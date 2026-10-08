/* Shared application/compiler directory creation at the Win32 boundary. */
#include "host_files.h"
#include "windows_host_path.h"

#if defined(_WIN32)
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int64_t hmje_file_error(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return -ENOENT;
        case ERROR_ACCESS_DENIED: return -EACCES;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return -EEXIST;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return -ENOMEM;
        case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return -EBUSY;
        case ERROR_FILENAME_EXCED_RANGE: return -36;
        default: return -EIO;
    }
}

static size_t hmje_root_length(const wchar_t *path) {
    size_t length = wcslen(path);
    if (length >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t at = 2;
        if (length >= 4 && path[2] == L'?' && path[3] == L'\\') {
            if (length < 8) return length;
            if (path[4] != L'U' || path[5] != L'N' || path[6] != L'C' ||
                path[7] != L'\\') return path[5] == L':' && path[6] == L'\\' ? 7 : 4;
            at = 8;
        }
        unsigned parts = 0;
        for (; path[at]; ++at) if (path[at] == L'\\' && ++parts == 2) return at + 1;
        return at;
    }
    if (length >= 3 && path[1] == L':' && path[2] == L'\\') return 3;
    return length && path[0] == L'\\' ? 1 : 0;
}

static int64_t hmje_make_directory(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return 0;
    DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
        return hmje_file_error(error);
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return -EEXIST;
    return 0;
}

int64_t hmje_host_directory_create_all(uint64_t path_address, uint64_t path_length) {
    wchar_t *path = hmje_windows_host_path(path_address, path_length);
    if (!path) return -EINVAL;
    size_t length = wcslen(path), root = hmje_root_length(path);
    int64_t result = 0;
    for (size_t i = root; i <= length; ++i) {
        if (i != length && path[i] != L'\\') continue;
        if (i == root) continue;
        wchar_t saved = path[i];
        path[i] = 0;
        result = hmje_make_directory(path);
        path[i] = saved;
        if (result) break;
    }
    free(path);
    return result;
}

#endif
