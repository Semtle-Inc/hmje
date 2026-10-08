
enum {
    HMJE_SQLITE_OPERATION_STAGE_NONE = 0,
    HMJE_SQLITE_OPERATION_STAGE_PATH = 1,
    HMJE_SQLITE_OPERATION_STAGE_SOURCE_OPEN = 2,
    HMJE_SQLITE_OPERATION_STAGE_DESTINATION_OPEN = 3,
    HMJE_SQLITE_OPERATION_STAGE_COPY_PREPARE = 4,
    HMJE_SQLITE_OPERATION_STAGE_COPY = 5,
    HMJE_SQLITE_OPERATION_STAGE_COPY_FINISH = 6,
    HMJE_SQLITE_OPERATION_STAGE_VERIFY = 7,
    HMJE_SQLITE_OPERATION_STAGE_SYNC = 8,
    HMJE_SQLITE_OPERATION_STAGE_CLOSE = 9,
    HMJE_SQLITE_OPERATION_STAGE_CLEANUP = 10,
    HMJE_SQLITE_OPERATION_STAGE_PUBLISH = 11
};

enum {
    HMJE_SQLITE_DESTINATION_UNCHANGED = 0,
    HMJE_SQLITE_DESTINATION_APPLIED = 1,
    HMJE_SQLITE_DESTINATION_CHECK_REQUIRED = 2
};

enum {
    HMJE_SQLITE_DURABILITY_UNKNOWN = 0,
    HMJE_SQLITE_DURABILITY_CONFIRMED = 1
};
static int verify_database(sqlite3 *database, int full,
                           uint64_t *problem_count) {
    sqlite3_stmt *statement = NULL;
    const char *sql = full != 0 ? "PRAGMA integrity_check"
                                : "PRAGMA quick_check";
    uint64_t rows = 0;
    uint64_t problems = 0;
    int result;
    int finalize_result;

    if (database == NULL || problem_count == NULL) {
        return SQLITE_MISUSE;
    }
    *problem_count = 0;
    /*
     * 검사 대상이 과거 연결에서 CHECK 제약 무시로 작성되었더라도 현재 검사
     * 연결의 설정 때문에 문제 행을 숨기지 않는다.
     */
    result = sqlite3_exec(database, "PRAGMA ignore_check_constraints=OFF",
                          NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    for (;;) {
        const unsigned char *message;
        result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            result = SQLITE_OK;
            break;
        }
        if (result != SQLITE_ROW) {
            break;
        }
        rows += 1;
        message = sqlite3_column_text(statement, 0);
        if (message == NULL) {
            result = sqlite3_column_type(statement, 0) == SQLITE_NULL
                         ? SQLITE_CORRUPT
                         : SQLITE_NOMEM;
            break;
        }
        if (strcmp((const char *)message, "ok") != 0) {
            problems += 1;
        }
    }
    finalize_result = sqlite3_finalize(statement);
    if (result == SQLITE_OK && finalize_result != SQLITE_OK) {
        result = finalize_result;
    }
    if (result == SQLITE_OK && problems == 0 && rows != 1) {
        problems += 1;
    }
    *problem_count = problems;
    return result;
}

static int copy_database(sqlite3 *destination, sqlite3 *source,
                         int pages_per_step, int lock_retry_limit,
                         uint64_t maximum_duration_ms,
                         int32_t *stage, uint64_t *page_count,
                         uint64_t *copied_pages,
                         int32_t *destination_state) {
    sqlite3_backup *backup;
    int result;
    int finish_result;
    int lock_retries = 0;
    uint64_t start_time = 0;

    *stage = HMJE_SQLITE_OPERATION_STAGE_COPY_PREPARE;
    if (maximum_duration_ms != 0) {
        result = monotonic_milliseconds(&start_time);
        if (result != SQLITE_OK) {
            return result;
        }
    }
    backup = sqlite3_backup_init(destination, "main", source, "main");
    if (backup == NULL) {
        result = sqlite3_errcode(destination);
        return result == SQLITE_OK ? SQLITE_ERROR : result;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_COPY;
    for (;;) {
        int total;
        int remaining;
        int primary_result;

        if (maximum_duration_ms != 0) {
            uint64_t current_time = 0;
            int clock_result = monotonic_milliseconds(&current_time);
            if (clock_result != SQLITE_OK || current_time < start_time) {
                result = SQLITE_IOERR;
                break;
            }
            if (current_time - start_time >= maximum_duration_ms) {
                result = SQLITE_BUSY;
                break;
            }
        }
        result = sqlite3_backup_step(backup, pages_per_step);
        total = sqlite3_backup_pagecount(backup);
        remaining = sqlite3_backup_remaining(backup);
        if (total >= 0) {
            *page_count = (uint64_t)total;
            if (remaining >= 0 && remaining <= total) {
                *copied_pages = (uint64_t)(total - remaining);
            }
        }
        if (maximum_duration_ms != 0) {
            uint64_t current_time = 0;
            int clock_result = monotonic_milliseconds(&current_time);
            if (clock_result != SQLITE_OK || current_time < start_time) {
                result = SQLITE_IOERR;
                break;
            }
            if (current_time - start_time >= maximum_duration_ms) {
                result = SQLITE_BUSY;
                break;
            }
        }
        if (result == SQLITE_DONE) {
            *destination_state = HMJE_SQLITE_DESTINATION_APPLIED;
            break;
        }
        if (result == SQLITE_OK) {
            lock_retries = 0;
            if (pages_per_step > 0) {
                (void)sqlite3_sleep(1);
            }
            continue;
        }
        primary_result = result & 0xff;
        if ((primary_result == SQLITE_BUSY ||
             primary_result == SQLITE_LOCKED) &&
            lock_retries < lock_retry_limit) {
            lock_retries += 1;
            (void)sqlite3_sleep(10);
            continue;
        }
        break;
    }

    finish_result = sqlite3_backup_finish(backup);
    if (result == SQLITE_DONE) {
        if (finish_result != SQLITE_OK) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_COPY_FINISH;
        }
        return finish_result;
    }
    if ((result & 0xff) != SQLITE_BUSY &&
        (result & 0xff) != SQLITE_LOCKED &&
        (result & 0xff) != SQLITE_READONLY) {
        *destination_state = HMJE_SQLITE_DESTINATION_CHECK_REQUIRED;
    }
    return result;
}

static int initialize_operation_outputs(
    uint64_t stage_address, uint64_t page_count_address,
    uint64_t copied_pages_address, uint64_t destination_state_address,
    uint64_t durability_state_address,
    int32_t **stage, uint64_t **page_count, uint64_t **copied_pages,
    int32_t **destination_state, int32_t **durability_state) {
    *stage = (int32_t *)(uintptr_t)stage_address;
    *page_count = (uint64_t *)(uintptr_t)page_count_address;
    *copied_pages = (uint64_t *)(uintptr_t)copied_pages_address;
    *destination_state =
        (int32_t *)(uintptr_t)destination_state_address;
    *durability_state =
        (int32_t *)(uintptr_t)durability_state_address;
    if (*stage == NULL || *page_count == NULL || *copied_pages == NULL ||
        *destination_state == NULL || *durability_state == NULL) {
        return SQLITE_MISUSE;
    }
    **stage = HMJE_SQLITE_OPERATION_STAGE_NONE;
    **page_count = 0;
    **copied_pages = 0;
    **destination_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
    **durability_state = HMJE_SQLITE_DURABILITY_UNKNOWN;
    return SQLITE_OK;
}

