#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hmje_sqlite_bridge.h"
#include "vendor/sqlite3.h"

/* The same generation-checked handle registry is used on every hosted target. */
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
#ifndef HMJE_SQLITE_TEST_COMMIT_FAULT_MODE
#define HMJE_SQLITE_TEST_COMMIT_FAULT_MODE 0
#endif
#ifndef HMJE_SQLITE_TEST_COMMIT_FAULT_AT
#define HMJE_SQLITE_TEST_COMMIT_FAULT_AT 0
#endif
#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE
static uint64_t hmje_sqlite_test_commit_count;
#endif
#include "hmje_sqlite_core.inc"
#include "hmje_sqlite_operations.inc"

static wchar_t *sqlite_windows_path(const char *path) {
    if (!path || !*path || !strncmp(path, "file:", 5) || !strcmp(path, ":memory:")) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0 || (size_t)n > SIZE_MAX / sizeof(wchar_t)) return NULL;
    wchar_t *wide = calloc((size_t)n, sizeof(wchar_t));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) { free(wide); return NULL; }
    return wide;
}

static int sqlite_windows_sidecars_absent(const char *path) {
    static const char *suffixes[] = {"-wal", "-shm", "-journal"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t n = strlen(path) + strlen(suffixes[i]) + 1;
        char *candidate = malloc(n);
        if (!candidate) return SQLITE_NOMEM;
        snprintf(candidate, n, "%s%s", path, suffixes[i]);
        wchar_t *wide = sqlite_windows_path(candidate);
        free(candidate);
        if (!wide) return SQLITE_CANTOPEN;
        DWORD attributes = GetFileAttributesW(wide);
        DWORD error = GetLastError();
        free(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES) return SQLITE_BUSY;
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return SQLITE_IOERR;
    }
    return SQLITE_OK;
}

static int sqlite_windows_backup(sqlite3 *source, const char *destination_path,
    int replace, uint64_t maximum_duration_ms, int32_t *stage,
    uint64_t *page_count, uint64_t *copied_pages, int32_t *destination_state,
    int32_t *durability_state) {
    sqlite3 *destination = NULL;
    HANDLE destination_lock = INVALID_HANDLE_VALUE;
    HANDLE temporary_file = INVALID_HANDLE_VALUE;
    wchar_t *destination_wide = sqlite_windows_path(destination_path);
    wchar_t *temporary_wide = NULL;
    char *temporary_path = NULL;
    int result = SQLITE_CANTOPEN;
    int temporary_created = 0;
    if (!source || !destination_wide) goto done;
    *stage = HMJE_SQLITE_OPERATION_STAGE_PATH;
    DWORD attributes = GetFileAttributesW(destination_wide);
    if (replace) {
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) goto done;
        result = sqlite_windows_sidecars_absent(destination_path);
        if (result != SQLITE_OK) goto done;
        /* Exclude live readers/writers while allowing our atomic replacement. */
        destination_lock = CreateFileW(destination_wide, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (destination_lock == INVALID_HANDLE_VALUE) { result = SQLITE_BUSY; goto done; }
    } else if (attributes != INVALID_FILE_ATTRIBUTES) { result = SQLITE_CANTOPEN; goto done; }
    size_t capacity = strlen(destination_path) + 80;
    temporary_path = malloc(capacity);
    if (!temporary_path) { result = SQLITE_NOMEM; goto done; }
    static volatile LONG serial;
    for (int attempt = 0; attempt < 32; attempt++) {
        snprintf(temporary_path, capacity, "%s.hmje-%lu-%ld.tmp", destination_path,
            GetCurrentProcessId(), InterlockedIncrement(&serial));
        free(temporary_wide);
        temporary_wide = sqlite_windows_path(temporary_path);
        if (!temporary_wide) { result = SQLITE_CANTOPEN; goto done; }
        temporary_file = CreateFileW(temporary_wide, GENERIC_READ | GENERIC_WRITE,
            0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (temporary_file != INVALID_HANDLE_VALUE) { temporary_created = 1; break; }
        if (GetLastError() != ERROR_FILE_EXISTS) { result = SQLITE_CANTOPEN; goto done; }
    }
    if (!temporary_created) { result = SQLITE_CANTOPEN; goto done; }
    CloseHandle(temporary_file); temporary_file = INVALID_HANDLE_VALUE;
    *stage = HMJE_SQLITE_OPERATION_STAGE_DESTINATION_OPEN;
    result = sqlite3_open_v2(temporary_path, &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) goto done;
    result = sqlite3_exec(destination, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL", NULL, NULL, NULL);
    if (result != SQLITE_OK) goto done;
    int32_t temporary_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
    result = copy_database(destination, source, 128, 500, maximum_duration_ms,
        stage, page_count, copied_pages, &temporary_state);
    if (result != SQLITE_OK) goto done;
    uint64_t problems = 0;
    *stage = HMJE_SQLITE_OPERATION_STAGE_VERIFY;
    result = verify_database(destination, 1, &problems);
    if (result != SQLITE_OK || problems) { if (result == SQLITE_OK) result = SQLITE_CORRUPT; goto done; }
    *stage = HMJE_SQLITE_OPERATION_STAGE_CLOSE;
    result = sqlite3_close(destination); destination = NULL;
    if (result != SQLITE_OK) goto done;
    *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
    temporary_file = CreateFileW(temporary_wide, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
    if (temporary_file == INVALID_HANDLE_VALUE || !FlushFileBuffers(temporary_file)) { result = SQLITE_IOERR; goto done; }
    CloseHandle(temporary_file); temporary_file = INVALID_HANDLE_VALUE;
    *stage = HMJE_SQLITE_OPERATION_STAGE_PUBLISH;
    if (replace) {
        /* POSIX rename semantics permit replacement while the checked old file
         * remains exclusively open. No unlock/rename window admits a writer. */
        temporary_file = CreateFileW(temporary_wide, DELETE | GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, NULL);
        size_t name_bytes = wcslen(destination_wide) * sizeof(wchar_t);
        size_t rename_bytes = offsetof(FILE_RENAME_INFO, FileName) + name_bytes;
        FILE_RENAME_INFO *rename_info = calloc(1, rename_bytes);
        if (temporary_file == INVALID_HANDLE_VALUE || !rename_info || rename_bytes > MAXDWORD) {
            free(rename_info); result = SQLITE_IOERR; goto done;
        }
        rename_info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename_info->FileNameLength = (DWORD)name_bytes;
        memcpy(rename_info->FileName, destination_wide, name_bytes);
        BOOL renamed = SetFileInformationByHandle(temporary_file, FileRenameInfoEx,
            rename_info, (DWORD)rename_bytes);
        free(rename_info);
        if (!renamed) { result = SQLITE_IOERR; goto done; }
        temporary_created = 0;
        *destination_state = HMJE_SQLITE_DESTINATION_APPLIED;
        if (!FlushFileBuffers(temporary_file)) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
            result = SQLITE_IOERR; goto done;
        }
        CloseHandle(temporary_file); temporary_file = INVALID_HANDLE_VALUE;
    } else if (!MoveFileExW(temporary_wide, destination_wide, MOVEFILE_WRITE_THROUGH)) {
        result = SQLITE_IOERR; goto done;
    }
    temporary_created = 0;
    *destination_state = HMJE_SQLITE_DESTINATION_APPLIED;
    *durability_state = HMJE_SQLITE_DURABILITY_CONFIRMED;
    *stage = HMJE_SQLITE_OPERATION_STAGE_NONE;
    result = SQLITE_OK;
done:
    if (destination) sqlite3_close(destination);
    if (temporary_file != INVALID_HANDLE_VALUE) CloseHandle(temporary_file);
    if (destination_lock != INVALID_HANDLE_VALUE) CloseHandle(destination_lock);
    if (temporary_created && temporary_wide) DeleteFileW(temporary_wide);
    free(destination_wide); free(temporary_wide); free(temporary_path);
    return result;
}

int32_t hmje_sqlite_online_backup(uint64_t database, uint64_t path_address,
    uint64_t path_length, uint64_t maximum_duration_ms, uint64_t stage_address,
    uint64_t page_count_address, uint64_t copied_pages_address,
    uint64_t destination_state_address, uint64_t durability_state_address) {
    int32_t *stage, *destination_state, *durability_state;
    uint64_t *page_count, *copied_pages;
    int result = initialize_operation_outputs(stage_address, page_count_address,
        copied_pages_address, destination_state_address, durability_state_address,
        &stage, &page_count, &copied_pages, &destination_state, &durability_state);
    if (result != SQLITE_OK) return result;
    char *path = NULL;
    result = copy_c_string(path_address, path_length, &path);
    if (result == SQLITE_OK) result = sqlite_windows_backup(database_from_handle(database),
        path, 0, maximum_duration_ms, stage, page_count, copied_pages, destination_state, durability_state);
    free(path); return result;
}

static int sqlite_windows_restore(uint64_t source_address, uint64_t source_length,
    uint64_t destination_address, uint64_t destination_length, int replace,
    uint64_t stage_address, uint64_t page_count_address, uint64_t copied_pages_address,
    uint64_t destination_state_address, uint64_t durability_state_address) {
    int32_t *stage, *destination_state, *durability_state;
    uint64_t *page_count, *copied_pages;
    int result = initialize_operation_outputs(stage_address, page_count_address,
        copied_pages_address, destination_state_address, durability_state_address,
        &stage, &page_count, &copied_pages, &destination_state, &durability_state);
    if (result != SQLITE_OK) return result;
    char *source_path = NULL, *destination_path = NULL;
    wchar_t *source_wide = NULL;
    HANDLE source_lock = INVALID_HANDLE_VALUE;
    sqlite3 *source = NULL;
    *stage = HMJE_SQLITE_OPERATION_STAGE_PATH;
    result = copy_c_string(source_address, source_length, &source_path);
    if (result != SQLITE_OK) goto done;
    result = copy_c_string(destination_address, destination_length, &destination_path);
    if (result != SQLITE_OK) goto done;
    source_wide = sqlite_windows_path(source_path);
    if (!source_wide) { result = SQLITE_CANTOPEN; goto done; }
    result = sqlite_windows_sidecars_absent(source_path);
    if (result != SQLITE_OK) goto done;
    source_lock = CreateFileW(source_wide, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (source_lock == INVALID_HANDLE_VALUE) { result = SQLITE_CANTOPEN; goto done; }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(source_lock, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { result = SQLITE_CANTOPEN; goto done; }
    *stage = HMJE_SQLITE_OPERATION_STAGE_SOURCE_OPEN;
    result = sqlite3_open_v2(source_path, &source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) goto done;
    uint64_t problems = 0;
    result = verify_database(source, 1, &problems);
    if (result != SQLITE_OK || problems) { if (result == SQLITE_OK) result = SQLITE_CORRUPT; goto done; }
    result = sqlite_windows_backup(source, destination_path, replace, 0, stage,
        page_count, copied_pages, destination_state, durability_state);
done:
    if (source) sqlite3_close(source);
    if (source_lock != INVALID_HANDLE_VALUE) CloseHandle(source_lock);
    free(source_path); free(destination_path); free(source_wide);
    return result;
}
#define RESTORE_EXPORT(name, replace) \
int32_t name(uint64_t a, uint64_t al, uint64_t b, uint64_t bl, uint64_t stage, \
    uint64_t pages, uint64_t copied, uint64_t state, uint64_t durability) { \
    return sqlite_windows_restore(a, al, b, bl, replace, stage, pages, copied, state, durability); }
RESTORE_EXPORT(hmje_sqlite_restore, 1)
RESTORE_EXPORT(hmje_sqlite_restore_new, 0)
