#ifndef HMJE_COMPILER_WINDOWS_HOST_PATH_H
#define HMJE_COMPILER_WINDOWS_HOST_PATH_H

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* UTF-8 source paths are not NUL-terminated. Extended Win32 paths require
 * backslashes; normalize separators only at the path boundary, never argv. */
static wchar_t *hmje_windows_host_path(uint64_t address, uint64_t length) {
    if (!address || !length || length >= 65536 || length > INT_MAX) return NULL;
    const char *bytes = (const char *)(uintptr_t)address;
    if (memchr(bytes, 0, (size_t)length)) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                    (int)length, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *path = malloc(((size_t)count + 1) * sizeof(wchar_t));
    if (!path) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length,
                            path, count) != count) {
        free(path);
        return NULL;
    }
    path[count] = 0;
    for (int i = 0; i < count; ++i) if (path[i] == L'/') path[i] = L'\\';
    return path;
}
#endif
#endif
