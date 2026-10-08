#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
typedef struct WatchEvent {
    struct WatchEvent *next;
    char *path;
    uint64_t kind;
} WatchEvent;
typedef struct WindowsWatch {
    CRITICAL_SECTION lock;
    HANDLE directory, changed, stopped, worker, waiter;
    WatchEvent *first, *last;
    size_t count;
    ULONGLONG last_ms;
    uint64_t delay_ms;
    char *root, *excludes;
    wchar_t *wide_root;
    int closing;
} WindowsWatch;

static char *span_copy(const uint8_t *bytes, uint64_t length) {
    if (!bytes || !length || length > 131072 || memchr(bytes, 0, (size_t)length)) return NULL;
    char *copy = malloc((size_t)length + 1);
    if (copy) { memcpy(copy, bytes, (size_t)length); copy[length] = 0; }
    return copy;
}
static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!length) return NULL;
    wchar_t *wide = malloc((size_t)length * sizeof(*wide));
    if (!wide || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        free(wide); return NULL;
    }
    return wide;
}
static char *to_utf8(const wchar_t *text, int chars) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, chars, NULL, 0, NULL, NULL);
    if (length <= 0) return NULL;
    char *bytes = malloc((size_t)length + 1);
    if (!bytes || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, chars,
                                     bytes, length, NULL, NULL) != length) { free(bytes); return NULL; }
    bytes[length] = 0;
    return bytes;
}
static int match_glob(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (pattern[1] == '*') pattern++;
            pattern++;
            if (!*pattern) return 1;
            do { if (match_glob(pattern, text)) return 1; } while (*text++);
            return 0;
        }
        if (*pattern != '?' && *pattern != *text) return 0;
        if (!*text) return 0;
        pattern++; text++;
    }
    return !*text;
}
static int excluded(WindowsWatch *watch, const char *relative) {
    if (!watch->excludes) return 0;
    const char *pattern = watch->excludes;
    while (*pattern) {
        const char *end = strchr(pattern, '\n');
        size_t length = end ? (size_t)(end - pattern) : strlen(pattern);
        if (length && length < 4096) {
            char copy[4096];
            memcpy(copy, pattern, length); copy[length] = 0;
            if (match_glob(copy, relative)) return 1;
            const char *part = relative;
            while (*part) {
                const char *slash = strchr(part, '/');
                size_t component = slash ? (size_t)(slash - part) : strlen(part);
                if (component < 4096) {
                    char name[4096];
                    memcpy(name, part, component); name[component] = 0;
                    if (match_glob(copy, name)) return 1;
                }
                if (!slash) break;
                part = slash + 1;
            }
        }
        if (!end) break;
        pattern = end + 1;
    }
    return 0;
}
static void push_event(WindowsWatch *watch, const wchar_t *name, int chars, uint64_t kind) {
    char *relative = to_utf8(name, chars);
    if (!relative) return;
    for (char *c = relative; *c; c++) if (*c == '\\') *c = '/';
    if (excluded(watch, relative)) { free(relative); return; }
    size_t root_length = strlen(watch->root), name_length = strlen(relative);
    char *path = malloc(root_length + name_length + 2);
    WatchEvent *event = malloc(sizeof(*event));
    if (!path || !event) { free(relative); free(path); free(event); return; }
    memcpy(path, watch->root, root_length);
    size_t offset = root_length;
    if (offset && path[offset - 1] != '/') path[offset++] = '/';
    memcpy(path + offset, relative, name_length + 1);
    free(relative);
    event->next = NULL; event->path = path; event->kind = kind;
    EnterCriticalSection(&watch->lock);
    if (watch->closing || watch->count >= 4096) { LeaveCriticalSection(&watch->lock); free(path); free(event); return; }
    if (watch->last) watch->last->next = event;
    else watch->first = event;
    watch->last = event; watch->count++;
    watch->last_ms = GetTickCount64();
    SetEvent(watch->changed);
    LeaveCriticalSection(&watch->lock);
}
static DWORD WINAPI watch_worker(void *pointer) {
    WindowsWatch *watch = pointer;
    BYTE buffer[65536];
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready) return 0;
    OVERLAPPED pending = {0}; pending.hEvent = ready;
    HANDLE waits[2] = {watch->stopped, ready};
    for (;;) {
        ResetEvent(ready);
        if (!ReadDirectoryChangesW(watch->directory, buffer, sizeof(buffer), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES,
            NULL, &pending, NULL) && GetLastError() != ERROR_IO_PENDING) break;
        DWORD signaled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signaled != WAIT_OBJECT_0 + 1) {
            CancelIoEx(watch->directory, &pending);
            DWORD ignored; GetOverlappedResult(watch->directory, &pending, &ignored, TRUE);
            break;
        }
        DWORD size = 0;
        if (!GetOverlappedResult(watch->directory, &pending, &size, FALSE)) break;
        for (DWORD offset = 0; size && offset < size;) {
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)(buffer + offset);
            if (info->FileNameLength > size - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName) ||
                info->FileNameLength % sizeof(wchar_t)) break;
            uint64_t kind = info->Action == FILE_ACTION_ADDED ? 1 :
                info->Action == FILE_ACTION_REMOVED ? 3 :
                (info->Action == FILE_ACTION_RENAMED_OLD_NAME ||
                 info->Action == FILE_ACTION_RENAMED_NEW_NAME) ? 4 : 2;
            push_event(watch, info->FileName, (int)(info->FileNameLength / sizeof(wchar_t)), kind);
            if (!info->NextEntryOffset || info->NextEntryOffset > size - offset) break;
            offset += info->NextEntryOffset;
        }
    }
    CloseHandle(ready);
    EnterCriticalSection(&watch->lock);
    watch->closing = 1;
    LeaveCriticalSection(&watch->lock);
    SetEvent(watch->stopped);
    SetEvent(watch->changed);
    return 0;
}
static DWORD WINAPI wait_worker(void *pointer) {
    struct WaitRequest { WindowsWatch *watch; uint64_t token; } *request = pointer;
    WindowsWatch *watch = request->watch;
    uint64_t token = request->token;
    free(request);
    int ready = 0;
    for (;;) {
        EnterCriticalSection(&watch->lock);
        if (watch->closing) { LeaveCriticalSection(&watch->lock); break; }
        ULONGLONG now = GetTickCount64();
        if (watch->first && now - watch->last_ms >= watch->delay_ms) {
            ready = 1; LeaveCriticalSection(&watch->lock); break;
        }
        DWORD timeout = watch->first ? (DWORD)(watch->delay_ms - (now - watch->last_ms)) : INFINITE;
        ResetEvent(watch->changed);
        LeaveCriticalSection(&watch->lock);
        HANDLE waits[2] = {watch->stopped, watch->changed};
        if (WaitForMultipleObjects(2, waits, FALSE, timeout) == WAIT_OBJECT_0) break;
    }
    hmje_async_complete_u64(token, 0, ready);
    return 0;
}
int64_t hmje_ui_watch_start(const uint8_t *root, uint64_t root_length,
    const uint8_t *excludes, uint64_t exclude_length, uint64_t delay_ms) {
    if (!root || !root_length || (exclude_length && !excludes) || delay_ms > 60000) return -EINVAL;
    char *path = span_copy(root, root_length);
    char *patterns = exclude_length ? span_copy(excludes, exclude_length) : NULL;
    if (!path || (exclude_length && !patterns)) { free(path); free(patterns); return -EINVAL; }
    wchar_t *wide = to_wide(path);
    if (!wide || !((wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/')) ||
                   (wide[0] == L'\\' && wide[1] == L'\\'))) {
        free(path); free(patterns); free(wide); return -EINVAL;
    }
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(path); free(patterns); free(wide); return -ENOENT;
    }
    WindowsWatch *watch = calloc(1, sizeof(*watch));
    if (!watch) { free(path); free(patterns); free(wide); return -ENOMEM; }
    InitializeCriticalSection(&watch->lock);
    watch->root = path; watch->excludes = patterns; watch->wide_root = wide; watch->delay_ms = delay_ms;
    for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
    watch->directory = CreateFileW(wide, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    watch->changed = CreateEventW(NULL, TRUE, FALSE, NULL);
    watch->stopped = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (watch->directory == INVALID_HANDLE_VALUE || !watch->changed || !watch->stopped ||
        !(watch->worker = CreateThread(NULL, 0, watch_worker, watch, 0, NULL))) {
        if (watch->directory != INVALID_HANDLE_VALUE) CloseHandle(watch->directory);
        if (watch->changed) CloseHandle(watch->changed);
        if (watch->stopped) CloseHandle(watch->stopped);
        DeleteCriticalSection(&watch->lock);
        free(path); free(patterns); free(wide); free(watch); return -EIO;
    }
    return (int64_t)(uintptr_t)watch;
}
int64_t hmje_ui_watch_stop(int64_t handle) {
    if (handle <= 0) return -EINVAL;
    WindowsWatch *watch = (WindowsWatch *)(uintptr_t)handle;
    EnterCriticalSection(&watch->lock); watch->closing = 1; LeaveCriticalSection(&watch->lock);
    SetEvent(watch->stopped);
    WaitForSingleObject(watch->worker, INFINITE);
    if (watch->waiter) WaitForSingleObject(watch->waiter, INFINITE);
    CloseHandle(watch->worker); if (watch->waiter) CloseHandle(watch->waiter);
    CloseHandle(watch->directory); CloseHandle(watch->changed); CloseHandle(watch->stopped);
    WatchEvent *event = watch->first;
    while (event) { WatchEvent *next = event->next; free(event->path); free(event); event = next; }
    DeleteCriticalSection(&watch->lock);
    free(watch->root); free(watch->excludes); free(watch->wide_root); free(watch);
    return 0;
}
int64_t hmje_ui_watch_next(int64_t handle, uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (handle <= 0 || !output || !written || capacity < 8) return -EINVAL;
    WindowsWatch *watch = (WindowsWatch *)(uintptr_t)handle;
    EnterCriticalSection(&watch->lock);
    if (watch->closing) { LeaveCriticalSection(&watch->lock); return -ECANCELED; }
    if (!watch->first || GetTickCount64() - watch->last_ms < watch->delay_ms) {
        LeaveCriticalSection(&watch->lock); return 0;
    }
    size_t bytes = 8;
    for (WatchEvent *event = watch->first; event; event = event->next) bytes += 16 + strlen(event->path);
    if (bytes > capacity) { LeaveCriticalSection(&watch->lock); return -ERANGE; }
    uint64_t count = watch->count;
    memcpy(output, &count, 8);
    size_t offset = 8;
    WatchEvent *event = watch->first;
    while (event) {
        uint64_t length = strlen(event->path);
        memcpy(output + offset, &event->kind, 8); offset += 8;
        memcpy(output + offset, &length, 8); offset += 8;
        memcpy(output + offset, event->path, (size_t)length); offset += (size_t)length;
        WatchEvent *next = event->next;
        free(event->path); free(event); event = next;
    }
    watch->first = watch->last = NULL; watch->count = 0;
    *written = offset;
    LeaveCriticalSection(&watch->lock);
    return 1;
}
void hmje_ui_watch_wait_async(int64_t handle, uint64_t token) {
    if (handle <= 0) { hmje_async_complete_u64(token, EINVAL, 0); return; }
    WindowsWatch *watch = (WindowsWatch *)(uintptr_t)handle;
    EnterCriticalSection(&watch->lock);
    if (watch->closing || (watch->waiter && WaitForSingleObject(watch->waiter, 0) == WAIT_TIMEOUT)) {
        LeaveCriticalSection(&watch->lock); hmje_async_complete_u64(token, EBUSY, 0); return;
    }
    if (watch->waiter) { CloseHandle(watch->waiter); watch->waiter = NULL; }
    struct WaitRequest { WindowsWatch *watch; uint64_t token; } *request = malloc(sizeof(*request));
    HANDLE created = NULL;
    if (request) { request->watch = watch; request->token = token;
        created = CreateThread(NULL, 0, wait_worker, request, 0, NULL);
        watch->waiter = created; }
    LeaveCriticalSection(&watch->lock);
    if (!created) { free(request); hmje_async_complete_u64(token, ENOMEM, 0); }
}
