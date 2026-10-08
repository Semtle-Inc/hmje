#include "host_directory_windows.c"

#if defined(_WIN32)
static volatile LONG64 hmje_scratch_sequence;

static wchar_t *hmje_remove_child_path(const wchar_t *directory, const wchar_t *name) {
    size_t a = wcslen(directory), b = wcslen(name);
    if (a > SIZE_MAX / sizeof(wchar_t) - b - 2) return NULL;
    wchar_t *child = malloc((a + b + 2) * sizeof(wchar_t));
    if (!child) return NULL;
    memcpy(child, directory, a * sizeof(wchar_t));
    child[a] = L'\\';
    memcpy(child + a + 1, name, (b + 1) * sizeof(wchar_t));
    return child;
}

static int64_t hmje_remove_work_tree(const wchar_t *path, unsigned depth) {
    if (depth > 256) return -ELOOP;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? 0 : hmje_file_error(error);
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return -EINVAL;
    wchar_t *pattern = hmje_remove_child_path(path, L"*");
    if (!pattern) return -ENOMEM;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) return hmje_file_error(error);
    } else {
        int64_t result = 0;
        do {
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                result = -ELOOP;
                break;
            }
            wchar_t *child = hmje_remove_child_path(path, entry.cFileName);
            if (!child) { result = -ENOMEM; break; }
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                result = hmje_remove_work_tree(child, depth + 1);
            else if (!DeleteFileW(child)) result = hmje_file_error(GetLastError());
            free(child);
            if (result) break;
        } while (FindNextFileW(search, &entry));
        if (!result && GetLastError() != ERROR_NO_MORE_FILES)
            result = hmje_file_error(GetLastError());
        FindClose(search);
        if (result) return result;
    }
    return RemoveDirectoryW(path) ? 0 : hmje_file_error(GetLastError());
}

int64_t hmje_host_directory_remove_tree(uint64_t path_address, uint64_t path_length) {
    wchar_t *path = hmje_windows_host_path(path_address, path_length);
    if (!path) return -EINVAL;
    size_t length = wcslen(path);
    while (length && path[length - 1] == L'\\') path[--length] = 0;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    int64_t result = wcsncmp(name, L"work-", 5) ? -EINVAL
                     : hmje_remove_work_tree(path, 0);
    free(path);
    return result;
}

uint64_t hmje_host_path_slot_hash(uint64_t path_address, uint64_t path_length) {
    if (!path_address) return 0;
    const unsigned char *bytes = (const unsigned char *)(uintptr_t)path_address;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint64_t i = 0; i < path_length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t hmje_host_file_content_hash(uint64_t path_address, uint64_t path_length) {
    wchar_t *path = hmje_windows_host_path(path_address, path_length);
    if (!path) return 0;
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return 0;
    unsigned char bytes[65536];
    uint64_t hash = UINT64_C(1469598103934665603);
    int failed = 0;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, bytes, sizeof(bytes), &count, NULL)) { failed = 1; break; }
        if (!count) break;
        for (DWORD i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    if (!CloseHandle(file)) failed = 1;
    return failed ? 0 : hash ? hash : 1;
}

static wchar_t *hmje_scratch_path(const wchar_t *target) {
    size_t length = wcslen(target);
    if (length > 32600 || length > SIZE_MAX / sizeof(wchar_t) - 64) return NULL;
    wchar_t *path = malloc((length + 64) * sizeof(wchar_t));
    if (!path) return NULL;
    long long sequence = (long long)InterlockedIncrement64(&hmje_scratch_sequence);
    int written = swprintf(path, length + 64, L"%ls.hmje-tmp-%lu-%lld",
                           target, (unsigned long)GetCurrentProcessId(), sequence);
    if (written < 0 || (size_t)written >= length + 64) { free(path); return NULL; }
    return path;
}

static HANDLE hmje_open_scratch(const wchar_t *target, wchar_t **scratch) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        wchar_t *path = hmje_scratch_path(target);
        if (!path) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) { *scratch = path; return file; }
        DWORD error = GetLastError();
        free(path);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetLastError(error);
            return INVALID_HANDLE_VALUE;
        }
    }
    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
}

static int64_t hmje_finish_scratch(HANDLE file, const wchar_t *scratch,
                                   const wchar_t *target, int64_t error) {
    if (!error && !FlushFileBuffers(file)) error = hmje_file_error(GetLastError());
    if (!CloseHandle(file) && !error) error = hmje_file_error(GetLastError());
    if (!error && !MoveFileExW(scratch, target,
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = hmje_file_error(GetLastError());
    if (error) DeleteFileW(scratch);
    return error;
}

static int64_t hmje_write_scratch(HANDLE file, const void *source, uint64_t length) {
    const unsigned char *bytes = source;
    uint64_t offset = 0;
    while (offset < length) {
        DWORD amount = length - offset > 65536 ? 65536 : (DWORD)(length - offset);
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, amount, &written, NULL))
            return hmje_file_error(GetLastError());
        if (!written) return -EIO;
        offset += written;
    }
    return 0;
}

int64_t hmje_compiler_cache_publish(uint64_t path_address, uint64_t path_length,
                                    uint64_t data_address, uint64_t data_length) {
    if ((!data_address && data_length) || data_length > SIZE_MAX) return -EINVAL;
    wchar_t *target = hmje_windows_host_path(path_address, path_length);
    if (!target) return -EINVAL;
    wchar_t *scratch = NULL;
    HANDLE file = hmje_open_scratch(target, &scratch);
    if (file == INVALID_HANDLE_VALUE) {
        int64_t error = hmje_file_error(GetLastError());
        free(scratch); free(target); return error;
    }
    int64_t error = hmje_write_scratch(file, (const void *)(uintptr_t)data_address,
                                       data_length);
    error = hmje_finish_scratch(file, scratch, target, error);
    free(scratch); free(target);
    return error;
}

int64_t hmje_host_file_copy_atomic(uint64_t source_address, uint64_t source_length,
                                   uint64_t target_address, uint64_t target_length,
                                   uint64_t executable) {
    (void)executable; /* Win32 has no POSIX execute-permission bit. */
    wchar_t *source = hmje_windows_host_path(source_address, source_length);
    wchar_t *target = hmje_windows_host_path(target_address, target_length);
    if (!source || !target) { free(source); free(target); return -EINVAL; }
    HANDLE input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) {
        int64_t error = hmje_file_error(GetLastError());
        free(source); free(target); return error;
    }
    wchar_t *scratch = NULL;
    HANDLE output = hmje_open_scratch(target, &scratch);
    if (output == INVALID_HANDLE_VALUE) {
        int64_t error = hmje_file_error(GetLastError());
        CloseHandle(input); free(source); free(target); free(scratch); return error;
    }
    unsigned char bytes[65536];
    int64_t error = 0;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(input, bytes, sizeof(bytes), &count, NULL)) {
            error = hmje_file_error(GetLastError()); break;
        }
        if (!count) break;
        error = hmje_write_scratch(output, bytes, count);
        if (error) break;
    }
    if (!CloseHandle(input) && !error) error = hmje_file_error(GetLastError());
    error = hmje_finish_scratch(output, scratch, target, error);
    free(source); free(target); free(scratch);
    return error;
}

int64_t hmje_host_file_replace_atomic(uint64_t source_address, uint64_t source_length,
                                      uint64_t target_address, uint64_t target_length) {
    wchar_t *source = hmje_windows_host_path(source_address, source_length);
    wchar_t *target = hmje_windows_host_path(target_address, target_length);
    if (!source || !target) { free(source); free(target); return -EINVAL; }
    int64_t result = MoveFileExW(source, target,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        ? 0 : hmje_file_error(GetLastError());
    free(source); free(target);
    return result;
}
/* macOS bundle publication is unavailable on a Windows host. */
int64_t hmje_host_bundle_publish_atomic(uint64_t source_address, uint64_t source_length,
                                        uint64_t target_address, uint64_t target_length) {
    (void)source_address; (void)source_length; (void)target_address; (void)target_length;
    return -ENOSYS;
}

#endif
