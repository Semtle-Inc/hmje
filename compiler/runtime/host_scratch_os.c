/* Compiler-only OS boundary: stat layouts, permission/errno macros and Win32
 * process handles. Scratch ownership, traversal and retention policy are HMJE. */
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include "windows_host_path.h"
#else
#include <unistd.h>
#include <signal.h>
#endif

int32_t hmje_compiler_directory_create(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = hmje_windows_host_path((uint64_t)(uintptr_t)path, strlen(path));
    if (!wide) return -1;
    int result = _wmkdir(wide);
    free(wide);
    return result;
#else
    return mkdir(path, 0700);
#endif
}

int32_t hmje_compiler_directory_remove(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = hmje_windows_host_path((uint64_t)(uintptr_t)path, strlen(path));
    if (!wide) return -1;
    int result = _wrmdir(wide);
    free(wide);
    return result;
#else
    return rmdir(path);
#endif
}

/* Unknown/inaccessible PIDs count as alive: never delete their scratch. */
int32_t hmje_compiler_process_alive(int32_t pid) {
    if (pid <= 0) return 1;
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD code = STILL_ACTIVE;
    int alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill(pid, 0) == 0 || errno != ESRCH;
#endif
}

/* Only real directories owned by this user qualify. Never follow symlinks or
 * Windows reparse points, including during recursive HMJE removal. */
int64_t hmje_compiler_directory_mtime(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = hmje_windows_host_path((uint64_t)(uintptr_t)path, strlen(path));
    if (!wide) return -1;
    DWORD attributes = GetFileAttributesW(wide);
    struct _stat64 info;
    int failed = attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) || _wstat64(wide, &info);
    free(wide);
    if (failed) return -1;
#else
    struct stat info;
    if (lstat(path, &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid()) return -1;
#endif
    return (int64_t)info.st_mtime;
}
