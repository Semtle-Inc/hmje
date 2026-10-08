/* Shared checked handles, bind/execute ABI, transactions and deadlines. */

/*
 * Never expose sqlite3 pointers as HMJE values.  A copied HMJE wrapper can
 * otherwise turn into a use-after-free after close/finalize.  Tokens contain
 * a slot index and generation, so forged, stale and double-released handles
 * fail closed.  The mutex protects registry publication and the one API that
 * is intentionally callable from a cancelling thread (interrupt).
 *
 * SQLite connections/statements remain owner-thread resources.  Registry
 * synchronization does not make arbitrary concurrent use of one connection
 * valid; it only makes handle validation and interrupt-vs-close well-defined.
 */
#define HMJE_SQLITE_HANDLE_SLOTS 16384u
#define HMJE_SQLITE_HANDLE_SLOT_BITS 15u
#define HMJE_SQLITE_HANDLE_INDEX_MASK \
    ((UINT64_C(1) << HMJE_SQLITE_HANDLE_SLOT_BITS) - UINT64_C(1))
#define HMJE_SQLITE_HANDLE_GENERATION_MAX \
    ((UINT64_C(1) << (63u - HMJE_SQLITE_HANDLE_SLOT_BITS)) - UINT64_C(1))

_Static_assert(
    HMJE_SQLITE_HANDLE_SLOTS <= HMJE_SQLITE_HANDLE_INDEX_MASK,
    "SQLite handle index must fit in the positive token"
);

enum {
    HMJE_SQLITE_HANDLE_DATABASE = 1,
    HMJE_SQLITE_HANDLE_STATEMENT = 2
};

typedef struct hmje_sqlite_handle_slot {
    void *pointer;
    uint64_t generation;
    int32_t busy_timeout_milliseconds;
    uint8_t type;
} hmje_sqlite_handle_slot;

static hmje_sqlite_handle_slot
    hmje_sqlite_handles[HMJE_SQLITE_HANDLE_SLOTS];
static pthread_mutex_t hmje_sqlite_handles_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t hmje_sqlite_handle_register(void *pointer, uint8_t type) {
    uint64_t token = 0;
    if (pointer == NULL || type == 0) {
        return 0;
    }
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return 0;
    }
    for (uint32_t index = 0; index < HMJE_SQLITE_HANDLE_SLOTS; ++index) {
        hmje_sqlite_handle_slot *slot = &hmje_sqlite_handles[index];
        if (slot->pointer != NULL ||
            slot->generation > HMJE_SQLITE_HANDLE_GENERATION_MAX) {
            continue;
        }
        if (slot->generation == 0) {
            slot->generation = 1;
        }
        slot->pointer = pointer;
        slot->busy_timeout_milliseconds = 0;
        slot->type = type;
        token = (slot->generation << HMJE_SQLITE_HANDLE_SLOT_BITS) |
            ((uint64_t)index + 1);
        break;
    }
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return token;
}

static void *hmje_sqlite_handle_resolve_locked(
    uint64_t handle,
    uint8_t type
) {
    uint64_t encoded_index = handle & HMJE_SQLITE_HANDLE_INDEX_MASK;
    uint64_t generation = handle >> HMJE_SQLITE_HANDLE_SLOT_BITS;
    hmje_sqlite_handle_slot *slot;
    if (encoded_index == 0 || encoded_index > HMJE_SQLITE_HANDLE_SLOTS ||
        generation == 0 ||
        generation > HMJE_SQLITE_HANDLE_GENERATION_MAX) {
        return NULL;
    }
    slot = &hmje_sqlite_handles[encoded_index - 1];
    if (slot->pointer == NULL || slot->type != type ||
        slot->generation != generation) {
        return NULL;
    }
    return slot->pointer;
}

static void *hmje_sqlite_handle_resolve(uint64_t handle, uint8_t type) {
    void *pointer;
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return NULL;
    }
    pointer = hmje_sqlite_handle_resolve_locked(handle, type);
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return pointer;
}

static int hmje_sqlite_handle_retire_locked(
    uint64_t handle,
    uint8_t type,
    void *expected_pointer
) {
    uint64_t encoded_index = handle & HMJE_SQLITE_HANDLE_INDEX_MASK;
    hmje_sqlite_handle_slot *slot;
    if (hmje_sqlite_handle_resolve_locked(handle, type) != expected_pointer) {
        return 0;
    }
    encoded_index = handle & HMJE_SQLITE_HANDLE_INDEX_MASK;
    slot = &hmje_sqlite_handles[encoded_index - 1];
    slot->pointer = NULL;
    slot->busy_timeout_milliseconds = 0;
    slot->type = 0;
    if (slot->generation < HMJE_SQLITE_HANDLE_GENERATION_MAX) {
        ++slot->generation;
    } else {
        /* Never let a stale token become valid again after generation wrap. */
        slot->generation = HMJE_SQLITE_HANDLE_GENERATION_MAX + UINT64_C(1);
    }
    return 1;
}

static sqlite3 *database_from_handle(uint64_t handle) {
    return (sqlite3 *)hmje_sqlite_handle_resolve(
        handle,
        HMJE_SQLITE_HANDLE_DATABASE
    );
}

static sqlite3_stmt *statement_from_handle(uint64_t handle) {
    return (sqlite3_stmt *)hmje_sqlite_handle_resolve(
        handle,
        HMJE_SQLITE_HANDLE_STATEMENT
    );
}

static void *pointer_from_address(uint64_t address) {
    return (void *)(uintptr_t)address;
}

static const char empty_text_value[] = "";
static const unsigned char empty_blob_value = 0;
static int copy_c_string(uint64_t address, uint64_t length, char **output) {
    const void *source = pointer_from_address(address);
    char *copy;

    if (output == NULL) {
        return SQLITE_MISUSE;
    }
    *output = NULL;
    if ((length != 0 && source == NULL) || length > (uint64_t)(SIZE_MAX - 1)) {
        return SQLITE_MISUSE;
    }
    if (length != 0 && memchr(source, '\0', (size_t)length) != NULL) {
        return SQLITE_MISUSE;
    }
    copy = (char *)malloc((size_t)length + 1);
    if (copy == NULL) {
        return SQLITE_NOMEM;
    }
    if (length != 0) {
        memcpy(copy, source, (size_t)length);
    }
    copy[length] = '\0';
    *output = copy;
    return SQLITE_OK;
}

static int copy_result(const void *source, int64_t length, uint64_t destination,
                       uint64_t capacity) {
    void *target = pointer_from_address(destination);

    if (length < 0) {
        return SQLITE_ERROR;
    }
    if ((uint64_t)length > capacity) {
        return SQLITE_RANGE;
    }
    if (length == 0) {
        return SQLITE_OK;
    }
    if (source == NULL || target == NULL) {
        return SQLITE_MISUSE;
    }
    memcpy(target, source, (size_t)length);
    return SQLITE_OK;
}

static int monotonic_milliseconds(uint64_t *milliseconds) {
#if defined(_WIN32)
    if (milliseconds == NULL) return SQLITE_MISUSE;
    *milliseconds = GetTickCount64();
    return SQLITE_OK;
#else
    struct timespec value;
    uint64_t seconds;
    uint64_t nanoseconds;
    if (milliseconds == NULL ||
        clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0 || value.tv_nsec < 0) {
        return SQLITE_IOERR;
    }
    seconds = (uint64_t)value.tv_sec;
    nanoseconds = (uint64_t)value.tv_nsec;
    if (seconds > UINT64_MAX / UINT64_C(1000)) {
        return SQLITE_IOERR;
    }
    *milliseconds =
        seconds * UINT64_C(1000) + nanoseconds / UINT64_C(1000000);
    return SQLITE_OK;
#endif
}

int32_t hmje_sqlite_monotonic_milliseconds(
    uint64_t out_milliseconds_address
) {
    uint64_t *out_milliseconds =
        (uint64_t *)(uintptr_t)out_milliseconds_address;
    if (out_milliseconds == NULL) {
        return SQLITE_MISUSE;
    }
    *out_milliseconds = 0;
    return monotonic_milliseconds(out_milliseconds);
}

typedef struct hmje_sqlite_deadline_context {
    uint64_t deadline_milliseconds;
    uint64_t last_milliseconds;
    int32_t original_busy_timeout_milliseconds;
    int busy_timeout_changed;
    int clock_failed;
} hmje_sqlite_deadline_context;

static int hmje_sqlite_deadline_progress(void *opaque) {
    hmje_sqlite_deadline_context *context =
        (hmje_sqlite_deadline_context *)opaque;
    uint64_t now = 0;
    if (context == NULL || monotonic_milliseconds(&now) != SQLITE_OK ||
        now < context->last_milliseconds) {
        if (context != NULL) {
            context->clock_failed = 1;
        }
        return 1;
    }
    context->last_milliseconds = now;
    return now >= context->deadline_milliseconds ? 1 : 0;
}

static int hmje_sqlite_deadline_begin(
    sqlite3 *connection,
    uint64_t timeout_milliseconds,
    int32_t configured_busy_timeout_milliseconds,
    hmje_sqlite_deadline_context *context
) {
    uint64_t now = 0;
    if (connection == NULL || context == NULL ||
        configured_busy_timeout_milliseconds < 0 ||
        timeout_milliseconds == 0 || timeout_milliseconds > INT32_MAX) {
        return SQLITE_MISUSE;
    }
    if (monotonic_milliseconds(&now) != SQLITE_OK ||
        now > UINT64_MAX - timeout_milliseconds) {
        return SQLITE_IOERR;
    }
    context->deadline_milliseconds = now + timeout_milliseconds;
    context->last_milliseconds = now;
    context->original_busy_timeout_milliseconds =
        configured_busy_timeout_milliseconds;
    context->busy_timeout_changed = 0;
    context->clock_failed = 0;
    /*
     * A progress handler only runs between VM opcodes.  Bound SQLite's own
     * lock sleep as well, otherwise a configured busy timeout could exceed
     * this call-local deadline before the progress callback runs again.
     */
    if ((uint64_t)configured_busy_timeout_milliseconds >
        timeout_milliseconds) {
        int busy_result = sqlite3_busy_timeout(
            connection,
            (int)timeout_milliseconds
        );
        if (busy_result != SQLITE_OK) {
            return busy_result;
        }
        context->busy_timeout_changed = 1;
    }
    /* A small fixed instruction interval bounds cancellation observation. */
    sqlite3_progress_handler(
        connection,
        1000,
        hmje_sqlite_deadline_progress,
        context
    );
    return SQLITE_OK;
}

static int hmje_sqlite_deadline_end(
    sqlite3 *connection,
    const hmje_sqlite_deadline_context *context,
    int operation_result
) {
    int restore_result = SQLITE_OK;
    int primary;
    uint64_t now = 0;
    sqlite3_progress_handler(connection, 0, NULL, NULL);
    if (context != NULL && context->busy_timeout_changed) {
        restore_result = sqlite3_busy_timeout(
            connection,
            context->original_busy_timeout_milliseconds
        );
    }
    if (context != NULL && context->clock_failed) {
        return SQLITE_IOERR;
    }
    if (context != NULL &&
        (monotonic_milliseconds(&now) != SQLITE_OK ||
         now < context->last_milliseconds)) {
        return SQLITE_IOERR;
    }
    if (context != NULL && now >= context->deadline_milliseconds) {
        primary = operation_result & 0xff;
        if (primary == SQLITE_OK || primary == SQLITE_BUSY ||
            primary == SQLITE_ROW || primary == SQLITE_DONE) {
            operation_result = SQLITE_INTERRUPT;
        }
    }
    primary = operation_result & 0xff;
    if ((primary == SQLITE_OK || primary == SQLITE_ROW ||
         primary == SQLITE_DONE) && restore_result != SQLITE_OK) {
        return restore_result;
    }
    return operation_result;
}

int32_t hmje_sqlite_open(uint64_t path_address, uint64_t path_length,
                         int32_t flags, uint64_t out_database_address) {
    uint64_t *out_database =
        (uint64_t *)(uintptr_t)out_database_address;
    sqlite3 *database = NULL;
    char *path = NULL;
    int result;

    if (out_database == NULL) {
        return SQLITE_MISUSE;
    }
    *out_database = 0;
    result = copy_c_string(path_address, path_length, &path);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_open_v2(path, &database, flags, NULL);
    free(path);
    /*
     * sqlite3_open_v2는 실패 시에도 오류 메시지를 가진 핸들을 줄 수 있다.
     * 호출자가 메시지를 읽고 close할 수 있도록 그 핸들도 돌려준다.
     */
    if (database != NULL) {
        *out_database = hmje_sqlite_handle_register(
            database,
            HMJE_SQLITE_HANDLE_DATABASE
        );
        if (*out_database == 0) {
            (void)sqlite3_close(database);
            return SQLITE_NOMEM;
        }
    }
    return result;
}

int32_t hmje_sqlite_database_valid(uint64_t database) {
    return database_from_handle(database) == NULL ? 0 : 1;
}

int32_t hmje_sqlite_close(uint64_t database) {
    sqlite3 *connection;
    int result;
    if (database == 0) {
        return SQLITE_OK;
    }
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    connection = (sqlite3 *)hmje_sqlite_handle_resolve_locked(
        database,
        HMJE_SQLITE_HANDLE_DATABASE
    );
    if (connection == NULL) {
        (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
        return SQLITE_MISUSE;
    }
    result = sqlite3_close(connection);
    if (result == SQLITE_OK) {
        (void)hmje_sqlite_handle_retire_locked(
            database,
            HMJE_SQLITE_HANDLE_DATABASE,
            connection
        );
    }
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return result;
}

int32_t hmje_sqlite_busy_timeout(uint64_t database, int32_t milliseconds) {
    sqlite3 *connection;
    hmje_sqlite_handle_slot *slot;
    uint64_t encoded_index;
    int result;
    if (milliseconds < 0 ||
        pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    connection = (sqlite3 *)hmje_sqlite_handle_resolve_locked(
        database,
        HMJE_SQLITE_HANDLE_DATABASE
    );
    if (connection == NULL) {
        (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
        return SQLITE_MISUSE;
    }
    result = sqlite3_busy_timeout(connection, milliseconds);
    if (result == SQLITE_OK) {
        encoded_index = database & HMJE_SQLITE_HANDLE_INDEX_MASK;
        slot = &hmje_sqlite_handles[encoded_index - 1];
        slot->busy_timeout_milliseconds = milliseconds;
    }
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return result;
}

static int hmje_sqlite_connection_deadline_state(
    uint64_t database,
    sqlite3 **connection,
    int32_t *busy_timeout_milliseconds
) {
    uint64_t encoded_index;
    if (connection == NULL || busy_timeout_milliseconds == NULL ||
        pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    *connection = (sqlite3 *)hmje_sqlite_handle_resolve_locked(
        database,
        HMJE_SQLITE_HANDLE_DATABASE
    );
    if (*connection == NULL) {
        (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
        return SQLITE_MISUSE;
    }
    encoded_index = database & HMJE_SQLITE_HANDLE_INDEX_MASK;
    *busy_timeout_milliseconds =
        hmje_sqlite_handles[encoded_index - 1].busy_timeout_milliseconds;
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return SQLITE_OK;
}

int32_t hmje_sqlite_disable_persist_wal(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    int disabled = 0;
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_file_control(
        connection,
        "main",
        SQLITE_FCNTL_PERSIST_WAL,
        &disabled
    );
}

int32_t hmje_sqlite_interrupt(uint64_t database) {
    sqlite3 *connection;
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    connection = (sqlite3 *)hmje_sqlite_handle_resolve_locked(
        database,
        HMJE_SQLITE_HANDLE_DATABASE
    );
    if (connection == NULL) {
        (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
        return SQLITE_MISUSE;
    }
    sqlite3_interrupt(connection);
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return SQLITE_OK;
}

int32_t hmje_sqlite_exec(uint64_t database, uint64_t sql_address,
                         uint64_t sql_length) {
    sqlite3 *connection = database_from_handle(database);
    char *sql = NULL;
    char *error_message = NULL;
    int result;

    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    result = copy_c_string(sql_address, sql_length, &sql);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_exec(connection, sql, NULL, NULL, &error_message);
    sqlite3_free(error_message);
    free(sql);
    return result;
}

int32_t hmje_sqlite_exec_deadline(
    uint64_t database,
    uint64_t sql_address,
    uint64_t sql_length,
    uint64_t timeout_milliseconds
) {
    sqlite3 *connection = NULL;
    hmje_sqlite_deadline_context deadline;
    int32_t busy_timeout_milliseconds = 0;
    char *sql = NULL;
    char *error_message = NULL;
    int result;

    result = hmje_sqlite_connection_deadline_state(
        database,
        &connection,
        &busy_timeout_milliseconds
    );
    if (result != SQLITE_OK) return result;
    result = copy_c_string(sql_address, sql_length, &sql);
    if (result != SQLITE_OK) {
        return result;
    }
    result = hmje_sqlite_deadline_begin(
        connection,
        timeout_milliseconds,
        busy_timeout_milliseconds,
        &deadline
    );
    if (result == SQLITE_OK) {
        result = sqlite3_exec(connection, sql, NULL, NULL, &error_message);
        result = hmje_sqlite_deadline_end(connection, &deadline, result);
    }
    sqlite3_free(error_message);
    free(sql);
    return result;
}

static int hmje_sqlite_prepare_raw(
    sqlite3 *connection,
    uint64_t sql_address,
    uint64_t sql_length,
    sqlite3_stmt **out_prepared
) {
    sqlite3_stmt *prepared = NULL;
    const char *sql = (const char *)(uintptr_t)sql_address;
    const char *tail = NULL;
    const char *end;
    int result;

    if (connection == NULL || out_prepared == NULL ||
        (sql_length != 0 && sql == NULL) || sql_length > (uint64_t)INT_MAX) {
        return SQLITE_MISUSE;
    }
    *out_prepared = NULL;
    if (sql_length != 0 && memchr(sql, '\0', (size_t)sql_length) != NULL) {
        return SQLITE_MISUSE;
    }
    if (sql_length == 0) {
        sql = empty_text_value;
    }
    result = sqlite3_prepare_v2(
        connection,
        sql,
        (int)sql_length,
        &prepared,
        &tail
    );
    if (result != SQLITE_OK) {
        if (prepared != NULL) {
            (void)sqlite3_finalize(prepared);
        }
        return result;
    }
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    end = sql + (size_t)sql_length;
    while (tail != NULL && tail < end &&
           (*tail == ' ' || *tail == '\t' || *tail == '\r' ||
            *tail == '\n' || *tail == '\f')) {
        tail += 1;
    }
    if (tail == NULL || tail != end) {
        (void)sqlite3_finalize(prepared);
        return SQLITE_MISUSE;
    }
    *out_prepared = prepared;
    return SQLITE_OK;
}

static int hmje_sqlite_publish_statement(
    sqlite3_stmt *prepared,
    uint64_t *out_statement
) {
    uint64_t handle;
    if (prepared == NULL || out_statement == NULL) {
        return SQLITE_MISUSE;
    }
    handle = hmje_sqlite_handle_register(
        prepared,
        HMJE_SQLITE_HANDLE_STATEMENT
    );
    if (handle == 0) {
        (void)sqlite3_finalize(prepared);
        return SQLITE_NOMEM;
    }
    *out_statement = handle;
    return SQLITE_OK;
}

int32_t hmje_sqlite_prepare(uint64_t database, uint64_t sql_address,
                            uint64_t sql_length,
                            uint64_t out_statement_address) {
    sqlite3 *connection = database_from_handle(database);
    uint64_t *out_statement =
        (uint64_t *)(uintptr_t)out_statement_address;
    sqlite3_stmt *prepared = NULL;
    int result;
    if (out_statement == NULL) {
        return SQLITE_MISUSE;
    }
    *out_statement = 0;
    result = hmje_sqlite_prepare_raw(
        connection,
        sql_address,
        sql_length,
        &prepared
    );
    if (result != SQLITE_OK) {
        return result;
    }
    return hmje_sqlite_publish_statement(prepared, out_statement);
}

int32_t hmje_sqlite_prepare_deadline(
    uint64_t database,
    uint64_t sql_address,
    uint64_t sql_length,
    uint64_t timeout_milliseconds,
    uint64_t out_statement_address
) {
    sqlite3 *connection = NULL;
    uint64_t *out_statement =
        (uint64_t *)(uintptr_t)out_statement_address;
    sqlite3_stmt *prepared = NULL;
    hmje_sqlite_deadline_context deadline;
    int32_t busy_timeout_milliseconds = 0;
    int result;

    if (out_statement == NULL) {
        return SQLITE_MISUSE;
    }
    *out_statement = 0;
    result = hmje_sqlite_connection_deadline_state(
        database,
        &connection,
        &busy_timeout_milliseconds
    );
    if (result != SQLITE_OK) {
        return result;
    }
    result = hmje_sqlite_deadline_begin(
        connection,
        timeout_milliseconds,
        busy_timeout_milliseconds,
        &deadline
    );
    if (result != SQLITE_OK) {
        return result;
    }
    result = hmje_sqlite_prepare_raw(
        connection,
        sql_address,
        sql_length,
        &prepared
    );
    result = hmje_sqlite_deadline_end(connection, &deadline, result);
    if (result != SQLITE_OK) {
        if (prepared != NULL) {
            (void)sqlite3_finalize(prepared);
        }
        return result;
    }
    return hmje_sqlite_publish_statement(prepared, out_statement);
}

int32_t hmje_sqlite_statement_valid(uint64_t statement) {
    return statement_from_handle(statement) == NULL ? 0 : 1;
}

int32_t hmje_sqlite_finalize(uint64_t statement) {
    sqlite3_stmt *prepared;
    int result;
    if (statement == 0) {
        return SQLITE_OK;
    }
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    prepared = (sqlite3_stmt *)hmje_sqlite_handle_resolve_locked(
        statement,
        HMJE_SQLITE_HANDLE_STATEMENT
    );
    if (prepared == NULL) {
        (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
        return SQLITE_MISUSE;
    }
    /* sqlite3_finalize destroys the statement for every return code. */
    (void)hmje_sqlite_handle_retire_locked(
        statement,
        HMJE_SQLITE_HANDLE_STATEMENT,
        prepared
    );
    result = sqlite3_finalize(prepared);
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    return result;
}

int32_t hmje_sqlite_reset(uint64_t statement) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_reset(prepared);
}

int32_t hmje_sqlite_clear_bindings(uint64_t statement) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_clear_bindings(prepared);
}

int32_t hmje_sqlite_bind_parameter_count(uint64_t statement) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return -1;
    }
    return sqlite3_bind_parameter_count(prepared);
}

int32_t hmje_sqlite_bind_null(uint64_t statement, int32_t index) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_bind_null(prepared, index);
}

int32_t hmje_sqlite_bind_int64(uint64_t statement, int32_t index,
                               int64_t value) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_bind_int64(prepared, index, value);
}

int32_t hmje_sqlite_bind_double(uint64_t statement, int32_t index,
                                double value) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_bind_double(prepared, index, value);
}

int32_t hmje_sqlite_bind_text(uint64_t statement, int32_t index,
                              uint64_t value_address, uint64_t value_length) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const char *value = (const char *)(uintptr_t)value_address;
    if (prepared == NULL || (value_length != 0 && value == NULL)) {
        return SQLITE_MISUSE;
    }
    if (value_length == 0) {
        value = empty_text_value;
    }
    return sqlite3_bind_text64(prepared, index, value, value_length,
                               SQLITE_TRANSIENT, SQLITE_UTF8);
}

int32_t hmje_sqlite_bind_blob(uint64_t statement, int32_t index,
                              uint64_t value_address, uint64_t value_length) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const void *value = pointer_from_address(value_address);
    if (prepared == NULL || (value_length != 0 && value == NULL)) {
        return SQLITE_MISUSE;
    }
    if (value_length == 0) {
        value = &empty_blob_value;
    }
    return sqlite3_bind_blob64(prepared, index, value, value_length,
                               SQLITE_TRANSIENT);
}

int32_t hmje_sqlite_step(uint64_t statement) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_step(prepared);
}

int32_t hmje_sqlite_step_deadline(
    uint64_t statement,
    uint64_t timeout_milliseconds
) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    sqlite3 *connection;
    hmje_sqlite_deadline_context deadline;
    int32_t busy_timeout_milliseconds = 0;
    int owner_found = 0;
    int result;
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    connection = sqlite3_db_handle(prepared);
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    if (pthread_mutex_lock(&hmje_sqlite_handles_mutex) != 0) {
        return SQLITE_MISUSE;
    }
    /* The owning connection is unique among live database handle slots. */
    for (uint32_t index = 0; index < HMJE_SQLITE_HANDLE_SLOTS; ++index) {
        hmje_sqlite_handle_slot *slot = &hmje_sqlite_handles[index];
        if (slot->pointer == connection &&
            slot->type == HMJE_SQLITE_HANDLE_DATABASE) {
            busy_timeout_milliseconds = slot->busy_timeout_milliseconds;
            owner_found = 1;
            break;
        }
    }
    (void)pthread_mutex_unlock(&hmje_sqlite_handles_mutex);
    if (!owner_found) {
        return SQLITE_MISUSE;
    }
    result = hmje_sqlite_deadline_begin(
        connection,
        timeout_milliseconds,
        busy_timeout_milliseconds,
        &deadline
    );
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_step(prepared);
    return hmje_sqlite_deadline_end(connection, &deadline, result);
}

int32_t hmje_sqlite_column_count(uint64_t statement) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL) {
        return 0;
    }
    return sqlite3_column_count(prepared);
}

int64_t hmje_sqlite_column_name_length(uint64_t statement, int32_t column) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const char *name;
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return -1;
    }
    name = sqlite3_column_name(prepared, column);
    return name == NULL ? -1 : (int64_t)strlen(name);
}

int32_t hmje_sqlite_column_name_copy(
    uint64_t statement,
    int32_t column,
    uint64_t destination,
    uint64_t capacity
) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const char *name;
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return SQLITE_RANGE;
    }
    name = sqlite3_column_name(prepared, column);
    if (name == NULL) {
        return SQLITE_NOMEM;
    }
    return copy_result(name, (int64_t)strlen(name), destination, capacity);
}

int32_t hmje_sqlite_column_type(uint64_t statement, int32_t column) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return 0;
    }
    return sqlite3_column_type(prepared, column);
}

int64_t hmje_sqlite_column_int64(uint64_t statement, int32_t column) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return 0;
    }
    return sqlite3_column_int64(prepared, column);
}

double hmje_sqlite_column_double(uint64_t statement, int32_t column) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return 0.0;
    }
    return sqlite3_column_double(prepared, column);
}

int64_t hmje_sqlite_column_bytes(uint64_t statement, int32_t column) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return -1;
    }
    return sqlite3_column_bytes(prepared, column);
}

int32_t hmje_sqlite_column_text_length(uint64_t statement, int32_t column,
                                       uint64_t out_length_address) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    uint64_t *out_length = (uint64_t *)(uintptr_t)out_length_address;
    const unsigned char *value;
    int initial_type;
    int length;

    if (out_length == NULL || prepared == NULL) {
        return SQLITE_MISUSE;
    }
    *out_length = 0;
    if (column < 0 || column >= sqlite3_column_count(prepared)) {
        return SQLITE_RANGE;
    }

    initial_type = sqlite3_column_type(prepared, column);
    value = sqlite3_column_text(prepared, column);
    length = sqlite3_column_bytes(prepared, column);
    if (length < 0) {
        return SQLITE_ERROR;
    }
    if (value == NULL && initial_type != SQLITE_NULL) {
        if (length == 0 &&
            (initial_type == SQLITE_BLOB || initial_type == SQLITE_TEXT)) {
            *out_length = 0;
            return SQLITE_OK;
        }
        return SQLITE_NOMEM;
    }
    *out_length = (uint64_t)length;
    return SQLITE_OK;
}

int32_t hmje_sqlite_column_text_copy(uint64_t statement, int32_t column,
                                     uint64_t destination,
                                     uint64_t capacity) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const unsigned char *value;
    int initial_type;
    int length;
    if (prepared == NULL) {
        return SQLITE_MISUSE;
    }
    if (column < 0 || column >= sqlite3_column_count(prepared)) {
        return SQLITE_RANGE;
    }
    initial_type = sqlite3_column_type(prepared, column);
    value = sqlite3_column_text(prepared, column);
    length = sqlite3_column_bytes(prepared, column);
    if (length < 0) {
        return SQLITE_ERROR;
    }
    if (value == NULL && initial_type != SQLITE_NULL) {
        if (length == 0 &&
            (initial_type == SQLITE_BLOB || initial_type == SQLITE_TEXT)) {
            return SQLITE_OK;
        }
        return SQLITE_NOMEM;
    }
    return copy_result(value, length, destination, capacity);
}

int32_t hmje_sqlite_column_blob_length(uint64_t statement, int32_t column,
                                       uint64_t out_length_address) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    uint64_t *out_length = (uint64_t *)(uintptr_t)out_length_address;
    const void *value;
    int initial_type;
    int length;

    if (out_length == NULL || prepared == NULL) {
        return SQLITE_MISUSE;
    }
    *out_length = 0;
    if (column < 0 || column >= sqlite3_column_count(prepared)) {
        return SQLITE_RANGE;
    }
    initial_type = sqlite3_column_type(prepared, column);
    value = sqlite3_column_blob(prepared, column);
    length = sqlite3_column_bytes(prepared, column);
    if (length < 0) {
        return SQLITE_ERROR;
    }
    if (value == NULL && initial_type != SQLITE_NULL) {
        if (length == 0 &&
            (initial_type == SQLITE_BLOB || initial_type == SQLITE_TEXT)) {
            *out_length = 0;
            return SQLITE_OK;
        }
        return SQLITE_NOMEM;
    }
    *out_length = (uint64_t)length;
    return SQLITE_OK;
}

int32_t hmje_sqlite_column_blob_copy(uint64_t statement, int32_t column,
                                     uint64_t destination,
                                     uint64_t capacity) {
    sqlite3_stmt *prepared = statement_from_handle(statement);
    const void *value;
    int initial_type;
    int length;
    if (prepared == NULL || column < 0 ||
        column >= sqlite3_column_count(prepared)) {
        return SQLITE_RANGE;
    }
    initial_type = sqlite3_column_type(prepared, column);
    value = sqlite3_column_blob(prepared, column);
    length = sqlite3_column_bytes(prepared, column);
    if (length < 0) {
        return SQLITE_ERROR;
    }
    if (value == NULL && initial_type != SQLITE_NULL) {
        if (length == 0 &&
            (initial_type == SQLITE_BLOB || initial_type == SQLITE_TEXT)) {
            return SQLITE_OK;
        }
        return SQLITE_NOMEM;
    }
    return copy_result(value, length, destination, capacity);
}

int64_t hmje_sqlite_changes(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return 0;
    }
    return sqlite3_changes64(connection);
}

int64_t hmje_sqlite_last_insert_id(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return 0;
    }
    return sqlite3_last_insert_rowid(connection);
}

int32_t hmje_sqlite_autocommit(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return 0;
    }
    return sqlite3_get_autocommit(connection);
}

int32_t hmje_sqlite_error_code(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_errcode(connection);
}

int32_t hmje_sqlite_extended_error_code(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_extended_errcode(connection);
}

int64_t hmje_sqlite_error_length(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    const char *message;
    if (connection == NULL) {
        return 0;
    }
    message = sqlite3_errmsg(connection);
    return message == NULL ? 0 : (int64_t)strlen(message);
}

int32_t hmje_sqlite_error_copy(uint64_t database, uint64_t destination,
                               uint64_t capacity) {
    sqlite3 *connection = database_from_handle(database);
    const char *message;
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    message = sqlite3_errmsg(connection);
    if (message == NULL) {
        return SQLITE_OK;
    }
    return copy_result(message, (int64_t)strlen(message), destination, capacity);
}

int32_t hmje_sqlite_begin(uint64_t database, int32_t mode) {
    sqlite3 *connection = database_from_handle(database);
    const char *sql;
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    switch (mode) {
        case 0:
            sql = "BEGIN DEFERRED";
            break;
        case 1:
            sql = "BEGIN IMMEDIATE";
            break;
        case 2:
            sql = "BEGIN EXCLUSIVE";
            break;
        default:
            return SQLITE_MISUSE;
    }
    return sqlite3_exec(connection, sql, NULL, NULL, NULL);
}

int32_t hmje_sqlite_commit(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    int32_t result;
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE != 0
    hmje_sqlite_test_commit_count += 1;
    if (
        hmje_sqlite_test_commit_count == HMJE_SQLITE_TEST_COMMIT_FAULT_AT &&
        HMJE_SQLITE_TEST_COMMIT_FAULT_MODE == 1
    ) {
        return SQLITE_IOERR;
    }
#endif
    result = sqlite3_exec(connection, "COMMIT", NULL, NULL, NULL);
#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE == 2
    if (
        hmje_sqlite_test_commit_count == HMJE_SQLITE_TEST_COMMIT_FAULT_AT &&
        result == SQLITE_OK
    ) {
        return SQLITE_IOERR;
    }
#endif
    return result;
}

int32_t hmje_sqlite_rollback(uint64_t database) {
    sqlite3 *connection = database_from_handle(database);
    if (connection == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_exec(connection, "ROLLBACK", NULL, NULL, NULL);
}

int32_t hmje_sqlite_savepoint(uint64_t database, int32_t operation,
                              uint64_t name_address, uint64_t name_length) {
    sqlite3 *connection = database_from_handle(database);
    char *name = NULL;
    char *sql = NULL;
    const char *format;
    int result;

    if (connection == NULL || name_length == 0) {
        return SQLITE_MISUSE;
    }
    result = copy_c_string(name_address, name_length, &name);
    if (result != SQLITE_OK) {
        return result;
    }
    switch (operation) {
        case 0:
            format = "SAVEPOINT \"%w\"";
            break;
        case 1:
            format = "RELEASE SAVEPOINT \"%w\"";
            break;
        case 2:
            format = "ROLLBACK TO SAVEPOINT \"%w\"";
            break;
        default:
            free(name);
            return SQLITE_MISUSE;
    }
    /* %w는 식별자 안의 큰따옴표를 두 번 써서 SQL 구문으로 탈출한다. */
    sql = sqlite3_mprintf(format, name);
    free(name);
    if (sql == NULL) {
        return SQLITE_NOMEM;
    }
    result = sqlite3_exec(connection, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    return result;
}

