#ifndef HMJE_SQLITE_BRIDGE_H
#define HMJE_SQLITE_BRIDGE_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * HMJE의 `주소`는 hosted LLVM ABI에서 uint64_t로 내려온다. 이 경계에서는
 * sqlite3/sqlite3_stmt 포인터를 노출하지 않고 동일한 폭의 불투명 핸들로만 주고받는다.
 */
int32_t hmje_sqlite_open(uint64_t path_address, uint64_t path_length,
                         int32_t flags, uint64_t out_database_address);
int32_t hmje_sqlite_database_valid(uint64_t database);
int32_t hmje_sqlite_close(uint64_t database);
int32_t hmje_sqlite_busy_timeout(uint64_t database, int32_t milliseconds);
int32_t hmje_sqlite_interrupt(uint64_t database);
int32_t hmje_sqlite_disable_persist_wal(uint64_t database);
int32_t hmje_sqlite_monotonic_milliseconds(uint64_t out_milliseconds_address);

int32_t hmje_sqlite_exec(uint64_t database, uint64_t sql_address,
                         uint64_t sql_length);
int32_t hmje_sqlite_exec_deadline(uint64_t database, uint64_t sql_address,
                                  uint64_t sql_length,
                                  uint64_t timeout_milliseconds);
/* Exactly one statement plus optional trailing ASCII whitespace. */
int32_t hmje_sqlite_prepare(uint64_t database, uint64_t sql_address,
                            uint64_t sql_length, uint64_t out_statement_address);
int32_t hmje_sqlite_prepare_deadline(uint64_t database,
                                     uint64_t sql_address,
                                     uint64_t sql_length,
                                     uint64_t timeout_milliseconds,
                                     uint64_t out_statement_address);
int32_t hmje_sqlite_statement_valid(uint64_t statement);
int32_t hmje_sqlite_finalize(uint64_t statement);
int32_t hmje_sqlite_reset(uint64_t statement);
int32_t hmje_sqlite_clear_bindings(uint64_t statement);
int32_t hmje_sqlite_bind_parameter_count(uint64_t statement);

int32_t hmje_sqlite_bind_null(uint64_t statement, int32_t index);
int32_t hmje_sqlite_bind_int64(uint64_t statement, int32_t index, int64_t value);
int32_t hmje_sqlite_bind_double(uint64_t statement, int32_t index, double value);
int32_t hmje_sqlite_bind_text(uint64_t statement, int32_t index,
                              uint64_t value_address, uint64_t value_length);
int32_t hmje_sqlite_bind_blob(uint64_t statement, int32_t index,
                              uint64_t value_address, uint64_t value_length);

int32_t hmje_sqlite_step(uint64_t statement);
int32_t hmje_sqlite_step_deadline(uint64_t statement,
                                  uint64_t timeout_milliseconds);
int32_t hmje_sqlite_column_count(uint64_t statement);
int64_t hmje_sqlite_column_name_length(uint64_t statement, int32_t column);
int32_t hmje_sqlite_column_name_copy(uint64_t statement, int32_t column,
                                     uint64_t destination,
                                     uint64_t capacity);
int32_t hmje_sqlite_column_type(uint64_t statement, int32_t column);
int64_t hmje_sqlite_column_int64(uint64_t statement, int32_t column);
double hmje_sqlite_column_double(uint64_t statement, int32_t column);
int64_t hmje_sqlite_column_bytes(uint64_t statement, int32_t column);
int32_t hmje_sqlite_column_text_length(uint64_t statement, int32_t column,
                                       uint64_t out_length_address);
int32_t hmje_sqlite_column_blob_length(uint64_t statement, int32_t column,
                                       uint64_t out_length_address);
int32_t hmje_sqlite_column_text_copy(uint64_t statement, int32_t column,
                                     uint64_t destination, uint64_t capacity);
int32_t hmje_sqlite_column_blob_copy(uint64_t statement, int32_t column,
                                     uint64_t destination, uint64_t capacity);

int64_t hmje_sqlite_changes(uint64_t database);
int64_t hmje_sqlite_last_insert_id(uint64_t database);
int32_t hmje_sqlite_autocommit(uint64_t database);
int32_t hmje_sqlite_error_code(uint64_t database);
int32_t hmje_sqlite_extended_error_code(uint64_t database);
int64_t hmje_sqlite_error_length(uint64_t database);
int32_t hmje_sqlite_error_copy(uint64_t database, uint64_t destination,
                               uint64_t capacity);

/* mode: 0=DEFERRED, 1=IMMEDIATE, 2=EXCLUSIVE */
int32_t hmje_sqlite_begin(uint64_t database, int32_t mode);
int32_t hmje_sqlite_commit(uint64_t database);
int32_t hmje_sqlite_rollback(uint64_t database);

/* operation: 0=SAVEPOINT, 1=RELEASE, 2=ROLLBACK TO */
int32_t hmje_sqlite_savepoint(uint64_t database, int32_t operation,
                              uint64_t name_address, uint64_t name_length);

/*
 * operation outputs:
 * stage: 0..11, destination state: 0=unchanged, 1=applied, 2=check required,
 * durability state: 0=unknown, 1=file/directory sync confirmed.
 */
int32_t hmje_sqlite_online_backup(
    uint64_t source_database, uint64_t destination_path_address,
    uint64_t destination_path_length, uint64_t maximum_duration_ms,
    uint64_t out_stage_address,
    uint64_t out_page_count_address, uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address);
int32_t hmje_sqlite_restore(
    uint64_t source_path_address, uint64_t source_path_length,
    uint64_t destination_path_address, uint64_t destination_path_length,
    uint64_t out_stage_address, uint64_t out_page_count_address,
    uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address);
int32_t hmje_sqlite_restore_new(
    uint64_t source_path_address, uint64_t source_path_length,
    uint64_t destination_path_address, uint64_t destination_path_length,
    uint64_t out_stage_address, uint64_t out_page_count_address,
    uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address);

#if defined(__cplusplus)
}
#endif

#endif
