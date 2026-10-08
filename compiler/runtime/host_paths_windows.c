#include "host_paths.h"
#include "windows_host_path.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The compiler's source root is canonicalized once; every descendant is
 * visited without following a reparse point. The app filesystem sandbox in
 * windows_shim.c is deliberately not used for compiler-host project input. */
struct hmje_paths {
    char **items;
    size_t count;
    size_t capacity;
};

static int64_t hmje_windows_error(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return -ENOENT;
        case ERROR_ACCESS_DENIED: return -EACCES;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return -ENOMEM;
        case ERROR_DIRECTORY: return -20;
        default: return -EIO;
    }
}

static char *hmje_wide_to_utf8(const wchar_t *wide) {
    const wchar_t *source = wide;
    size_t prefix = 0;
    if (wcsncmp(wide, L"\\\\?\\UNC\\", 8) == 0) {
        source = wide + 8;
        prefix = 2;
    } else if (wcsncmp(wide, L"\\\\?\\", 4) == 0 &&
               wide[4] && wide[5] == L':' && wide[6] == L'\\') {
        source = wide + 4;
    } else if (wide[0] == L'\\' && wide[1] == L'\\' &&
               wide[2] != L'?') {
        source = wide + 2;
        prefix = 2;
    }
    size_t length = wcslen(source);
    if (length > INT_MAX) return NULL;
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source,
                                     (int)length, NULL, 0, NULL, NULL);
    if (needed <= 0) return NULL;
    char *bytes = malloc((size_t)needed + prefix + 1);
    if (!bytes) return NULL;
    if (prefix) { bytes[0] = '/'; bytes[1] = '/'; }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, (int)length,
                            bytes + prefix, needed, NULL, NULL) != needed) {
        free(bytes);
        return NULL;
    }
    for (size_t i = prefix; i < (size_t)needed + prefix; ++i)
        if (bytes[i] == '\\') bytes[i] = '/';
    bytes[(size_t)needed + prefix] = '\0';
    return bytes;
}

static wchar_t *hmje_final_path(uint64_t address, uint64_t length) {
    wchar_t *input = hmje_windows_host_path(address, length);
    if (!input) return NULL;
    HANDLE handle = CreateFileW(input, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(input);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    DWORD needed = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    wchar_t *result = NULL;
    if (needed > 0 && needed < 65536) {
        result = malloc(((size_t)needed + 1) * sizeof(wchar_t));
        if (result) {
            DWORD written = GetFinalPathNameByHandleW(handle, result, needed + 1,
                                                       FILE_NAME_NORMALIZED);
            if (written == 0 || written > needed) {
                free(result);
                result = NULL;
            }
        }
    }
    CloseHandle(handle);
    return result;
}

int64_t hmje_host_realpath(uint64_t input_address, uint64_t input_length,
                           uint64_t output_address, uint64_t output_capacity) {
    if (!output_address || !output_capacity) return -1;
    wchar_t *path = hmje_final_path(input_address, input_length);
    if (!path) return -1;
    char *bytes = hmje_wide_to_utf8(path);
    free(path);
    if (!bytes) return -1;
    size_t length = strlen(bytes);
    if (length >= output_capacity || length > INT64_MAX) {
        free(bytes);
        return -1;
    }
    memcpy((void *)(uintptr_t)output_address, bytes, length);
    free(bytes);
    return (int64_t)length;
}

static void hmje_paths_free(struct hmje_paths *paths) {
    for (size_t i = 0; i < paths->count; ++i) free(paths->items[i]);
    free(paths->items);
}

static int64_t hmje_paths_add(struct hmje_paths *paths, const wchar_t *wide,
                              int source_only) {
    char *bytes = hmje_wide_to_utf8(wide);
    if (!bytes) return -ENOMEM;
    size_t length = strlen(bytes);
    if (source_only && (length < 5 || strcmp(bytes + length - 5, ".hmje"))) {
        free(bytes);
        return 0;
    }
    if (strchr(bytes, '\n') || strchr(bytes, '\r')) {
        free(bytes);
        return -EINVAL;
    }
    if (paths->count == paths->capacity) {
        size_t next = paths->capacity ? paths->capacity * 2 : 16;
        if (next < paths->capacity || next > SIZE_MAX / sizeof(char *)) {
            free(bytes);
            return -75;
        }
        char **items = realloc(paths->items, next * sizeof(char *));
        if (!items) { free(bytes); return -ENOMEM; }
        paths->items = items;
        paths->capacity = next;
    }
    paths->items[paths->count++] = bytes;
    return 0;
}

static wchar_t *hmje_child_path(const wchar_t *directory, const wchar_t *name) {
    size_t a = wcslen(directory), b = wcslen(name);
    if (a > (SIZE_MAX / sizeof(wchar_t)) - b - 2) return NULL;
    wchar_t *path = malloc((a + b + 2) * sizeof(wchar_t));
    if (!path) return NULL;
    memcpy(path, directory, a * sizeof(wchar_t));
    path[a] = L'\\';
    memcpy(path + a + 1, name, (b + 1) * sizeof(wchar_t));
    return path;
}

static int64_t hmje_collect_paths(const wchar_t *root, struct hmje_paths *paths,
                                  int source_only, unsigned depth) {
    if (depth > 256) return -40;
    wchar_t *pattern = hmje_child_path(root, L"*");
    if (!pattern) return -ENOMEM;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? 0 : hmje_windows_error(error);
    }
    int64_t status = 0;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            status = -40;
            break;
        }
        wchar_t *child = hmje_child_path(root, entry.cFileName);
        if (!child) { status = -ENOMEM; break; }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            status = hmje_collect_paths(child, paths, source_only, depth + 1);
        else
            status = hmje_paths_add(paths, child, source_only);
        free(child);
        if (status) break;
    } while (FindNextFileW(search, &entry));
    if (!status) {
        DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) status = hmje_windows_error(error);
    }
    FindClose(search);
    return status;
}

static int hmje_compare_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int64_t hmje_list_paths(uint64_t input_address, uint64_t input_length,
                               uint64_t output_address, uint64_t output_capacity,
                               int source_only) {
    if (!input_address || !input_length || input_length >= 65536) return -EINVAL;
    wchar_t *root = hmje_final_path(input_address, input_length);
    if (!root) return -ENOENT;
    DWORD attributes = GetFileAttributesW(root);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        int64_t error = hmje_windows_error(GetLastError());
        free(root);
        return error;
    }
    struct hmje_paths paths = {0};
    int64_t status;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        status = hmje_collect_paths(root, &paths, source_only, 0);
    else
        status = hmje_paths_add(&paths, root, source_only);
    free(root);
    if (status) { hmje_paths_free(&paths); return status; }
    qsort(paths.items, paths.count, sizeof(char *), hmje_compare_paths);
    size_t required = 0;
    for (size_t i = 0; i < paths.count; ++i) {
        size_t length = strlen(paths.items[i]);
        if (required > INT64_MAX - length - 1) {
            hmje_paths_free(&paths);
            return -75;
        }
        required += length + 1;
    }
    if (!output_address) {
        hmje_paths_free(&paths);
        return (int64_t)required;
    }
    if (output_capacity < required) { hmje_paths_free(&paths); return -ENOSPC; }
    char *output = (char *)(uintptr_t)output_address;
    size_t offset = 0;
    for (size_t i = 0; i < paths.count; ++i) {
        size_t length = strlen(paths.items[i]);
        memcpy(output + offset, paths.items[i], length);
        offset += length;
        output[offset++] = '\n';
    }
    hmje_paths_free(&paths);
    return (int64_t)required;
}

int64_t hmje_host_list_hmje_sources(uint64_t input_address, uint64_t input_length,
                                    uint64_t output_address, uint64_t output_capacity) {
    return hmje_list_paths(input_address, input_length, output_address,
                           output_capacity, 1);
}

int64_t hmje_host_list_project_files(uint64_t input_address, uint64_t input_length,
                                     uint64_t output_address, uint64_t output_capacity) {
    return hmje_list_paths(input_address, input_length, output_address,
                           output_capacity, 0);
}
#endif
