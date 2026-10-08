/* Desktop window session state, shared by Cocoa, Win32 and X11 hosts.
 * Each application gets one bounded, atomically replaced state file. The file
 * contains only value-input windows marked active by the compiler, plus the
 * last frame for every named window. Closed windows retain their frame but
 * lose their active bit. A quit preserves active bits for the next launch. */
#ifndef HMJE_WINDOW_PERSIST_H
#define HMJE_WINDOW_PERSIST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define HMJE_PERSIST_MAX_RECORDS 256
#define HMJE_PERSIST_MAX_TEXT 65536
typedef struct hmje_persist_record {
    uint64_t runtime_id;
    char *key, *payload;
    uint32_t key_len, payload_len;
    int32_t x, y, width, height;
    uint32_t has_frame, active;
} hmje_persist_record;
static hmje_persist_record hmje_persist_records[HMJE_PERSIST_MAX_RECORDS];
static uint32_t hmje_persist_count;
static int hmje_persist_loaded;
/* Generated from the project manifest by the product linker. */
#ifdef _WIN32
extern const unsigned char hmje_window_app_identity[];
#else
/* Old bootstrap linkers predate the generated identity object. */
#ifndef HMJE_PERSIST_EXTERNAL_IDENTITY
__attribute__((weak)) const unsigned char hmje_window_app_identity[] = "";
#else
extern const unsigned char hmje_window_app_identity[];
#endif
#endif
#ifdef _WIN32
static wchar_t hmje_persist_path[32768];
#else
static char hmje_persist_path[4096];
#endif

static uint64_t hmje_persist_hash(const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static int hmje_persist_component(const char *name) {
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p == '/' || *p == '\\' || *p < 32
#ifdef _WIN32
            || strchr("<>:\"|?*", *p) != NULL
#endif
        ) return 0;
#ifdef _WIN32
    size_t length = strlen(name);
    if (name[length - 1] == '.' || name[length - 1] == ' ') return 0;
#endif
    return 1;
}
static FILE *hmje_persist_open(const char *mode, int temporary);
#ifdef _WIN32
static int hmje_persist_directory(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return 1;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
#else
static int hmje_persist_directory(const char *path) {
    if (mkdir(path, 0700) == 0) return 1;
    if (errno != EEXIST) return 0;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
static int hmje_persist_directories(char *path) {
    for (char *p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        int ok = hmje_persist_directory(path);
        *p = '/';
        if (!ok) return 0;
    }
    return hmje_persist_directory(path);
}
#endif
static void hmje_persist_migrate(
#ifdef _WIN32
    const wchar_t *old_path
#else
    const char *old_path
#endif
) {
#ifdef _WIN32
    FILE *existing = _wfopen(hmje_persist_path, L"rb");
    FILE *old = _wfopen(old_path, L"rb");
#else
    FILE *existing = fopen(hmje_persist_path, "rb");
    FILE *old = fopen(old_path, "rb");
#endif
    if (existing) { fclose(existing); if (old) fclose(old); return; }
    if (!old) return;
    FILE *dest = hmje_persist_open("wb", 1);
    if (!dest) { fclose(old); return; }
    unsigned char buffer[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buffer, 1, sizeof(buffer), old)) != 0)
        if (fwrite(buffer, 1, n, dest) != n) { ok = 0; break; }
    if (ferror(old)) ok = 0;
    fclose(old);
    if (fclose(dest) != 0) ok = 0;
#ifdef _WIN32
    wchar_t temp[32768]; wcscpy_s(temp, 32768, hmje_persist_path); wcscat_s(temp, 32768, L".tmp");
    if (ok && MoveFileExW(temp, hmje_persist_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(old_path);
    else DeleteFileW(temp);
#else
    char temp[4096]; snprintf(temp, sizeof(temp), "%s.tmp", hmje_persist_path);
    if (ok && rename(temp, hmje_persist_path) == 0) unlink(old_path);
    else unlink(temp);
#endif
}
static FILE *hmje_persist_open(const char *mode, int temporary) {
#ifdef _WIN32
    wchar_t path[32768];
    wcscpy_s(path, 32768, hmje_persist_path);
    if (temporary) wcscat_s(path, 32768, L".tmp");
    return _wfopen(path, mode[0] == 'r' ? L"rb" : L"wb");
#else
    char path[4096];
    snprintf(path, sizeof(path), "%s%s", hmje_persist_path, temporary ? ".tmp" : "");
    return fopen(path, mode);
#endif
}
static void hmje_persist_save(void) {
    if (!hmje_persist_loaded || !hmje_persist_path[0]) return;
    FILE *file = hmje_persist_open("wb", 1);
    if (!file) return;
    const uint32_t header[3] = {UINT32_C(0x484d574e), 1, hmje_persist_count};
    int ok = fwrite(header, sizeof(header), 1, file) == 1;
    for (uint32_t i = 0; ok && i < hmje_persist_count; ++i) {
        hmje_persist_record *record = &hmje_persist_records[i];
        int32_t info[8] = {(int32_t)record->key_len, (int32_t)record->payload_len,
                           record->x, record->y, record->width, record->height,
                           (int32_t)record->has_frame, (int32_t)record->active};
        ok = fwrite(info, sizeof(info), 1, file) == 1 &&
             fwrite(record->key, 1, record->key_len, file) == record->key_len &&
             fwrite(record->payload, 1, record->payload_len, file) == record->payload_len;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok) return;
#ifdef _WIN32
    wchar_t temp[32768]; wcscpy_s(temp, 32768, hmje_persist_path); wcscat_s(temp, 32768, L".tmp");
    MoveFileExW(temp, hmje_persist_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    char temp[4096]; snprintf(temp, sizeof(temp), "%s.tmp", hmje_persist_path);
    rename(temp, hmje_persist_path);
#endif
}
static void hmje_persist_init(void) {
    if (hmje_persist_loaded) return;
    hmje_persist_loaded = 1;
#ifdef _WIN32
    wchar_t executable[32768], directory[32768], old_path[32768], identity[1024];
    DWORD exe_length = GetModuleFileNameW(NULL, executable, 32768);
    DWORD dir_length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
    if (!exe_length || exe_length >= 32768 || !dir_length || dir_length >= 32000 ||
        !hmje_persist_component((const char *)hmje_window_app_identity)) return;
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        (const char *)hmje_window_app_identity, -1, identity, 1024);
    if (!converted) return;
    uint64_t hash = hmje_persist_hash(executable, exe_length * sizeof(wchar_t));
    swprintf_s(old_path, 32768, L"%ls\\hmje-window-%016llx.bin", directory,
               (unsigned long long)hash);
    if (swprintf_s(hmje_persist_path, 32768, L"%ls\\%ls\\window-state.bin", directory, identity) < 0) return;
    wchar_t app_dir[32768];
    if (swprintf_s(app_dir, 32768, L"%ls\\%ls", directory, identity) < 0 ||
        !hmje_persist_directory(app_dir)) return;
#else
    char executable[4096] = {0};
#ifdef __APPLE__
    uint32_t capacity = sizeof(executable);
    if (_NSGetExecutablePath(executable, &capacity) != 0) return;
#else
    ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n <= 0) return;
    executable[n] = 0;
#endif
    const char *directory = getenv("HOME");
    /* Old bootstrap linkers do not emit identity yet. Their compiler tools do
     * not persist windows; use the executable name until the new linker runs. */
    const char *identity = *hmje_window_app_identity ? (const char *)hmje_window_app_identity : strrchr(executable, '/');
    if (!*hmje_window_app_identity && identity) ++identity;
    if (!directory || strlen(directory) > 3500 || !hmje_persist_component(identity)) return;
    uint64_t hash = hmje_persist_hash(executable, strlen(executable));
    char old_path[4096], app_dir[4096];
    snprintf(old_path, sizeof(old_path), "%s/.hmje-window-%016llx.bin", directory,
             (unsigned long long)hash);
#ifdef __APPLE__
    int length = snprintf(app_dir, sizeof(app_dir), "%s/Library/Application Support/%s", directory, identity);
#else
    const char *state_home = getenv("XDG_STATE_HOME");
    char fallback[4096];
    if (!state_home || !*state_home) {
        snprintf(fallback, sizeof(fallback), "%s/.local/state", directory);
        state_home = fallback;
    }
    int length = snprintf(app_dir, sizeof(app_dir), "%s/%s", state_home, identity);
#endif
    if (length < 0 || (size_t)length >= sizeof(app_dir) || !hmje_persist_directories(app_dir)) return;
    if (snprintf(hmje_persist_path, sizeof(hmje_persist_path), "%s/window-state.bin", app_dir) >=
        (int)sizeof(hmje_persist_path)) return;
#endif
    hmje_persist_migrate(old_path);
    FILE *file = hmje_persist_open("rb", 0);
    if (!file) return;
    uint32_t header[3];
    if (fread(header, sizeof(header), 1, file) != 1 || header[0] != UINT32_C(0x484d574e) ||
        header[1] != 1 || header[2] > HMJE_PERSIST_MAX_RECORDS) { fclose(file); return; }
    for (uint32_t i = 0; i < header[2]; ++i) {
        int32_t info[8];
        if (fread(info, sizeof(info), 1, file) != 1 || info[0] <= 0 ||
            info[0] > HMJE_PERSIST_MAX_TEXT || info[1] < 0 || info[1] > HMJE_PERSIST_MAX_TEXT) break;
        char *key = (char *)malloc((size_t)info[0] + 1);
        char *payload = (char *)malloc((size_t)info[1] + 1);
        if (!key || !payload || fread(key, 1, (size_t)info[0], file) != (size_t)info[0] ||
            fread(payload, 1, (size_t)info[1], file) != (size_t)info[1]) {
            free(key); free(payload); break;
        }
        key[info[0]] = 0; payload[info[1]] = 0;
        hmje_persist_record *record = &hmje_persist_records[hmje_persist_count++];
        record->key = key; record->payload = payload;
        record->key_len = (uint32_t)info[0]; record->payload_len = (uint32_t)info[1];
        record->x = info[2]; record->y = info[3]; record->width = info[4]; record->height = info[5];
        record->has_frame = info[6] != 0; record->active = info[7] != 0;
    }
    fclose(file);
}
static hmje_persist_record *hmje_persist_find(const char *key, size_t length) {
    hmje_persist_init();
    for (uint32_t i = 0; i < hmje_persist_count; ++i) {
        hmje_persist_record *record = &hmje_persist_records[i];
        if (record->key_len == length && memcmp(record->key, key, length) == 0) return record;
    }
    return NULL;
}
static hmje_persist_record *hmje_persist_bind(uint64_t runtime_id, const char *key, size_t key_len,
                                               const char *payload, size_t payload_len, int active) {
    if (!key || !key_len || key_len > HMJE_PERSIST_MAX_TEXT || payload_len > HMJE_PERSIST_MAX_TEXT ||
        (!payload && payload_len)) return NULL;
    hmje_persist_record *record = hmje_persist_find(key, key_len);
    if (!record) {
        if (hmje_persist_count == HMJE_PERSIST_MAX_RECORDS) return NULL;
        record = &hmje_persist_records[hmje_persist_count++];
        record->key = (char *)malloc(key_len + 1);
        if (!record->key) { --hmje_persist_count; return NULL; }
        memcpy(record->key, key, key_len); record->key[key_len] = 0;
        record->key_len = (uint32_t)key_len;
    }
    char *copy = (char *)malloc(payload_len + 1);
    if (!copy) return NULL;
    if (payload_len) memcpy(copy, payload, payload_len);
    copy[payload_len] = 0;
    free(record->payload); record->payload = copy;
    record->payload_len = (uint32_t)payload_len;
    record->runtime_id = runtime_id;
    record->active = active != 0;
    hmje_persist_save();
    return record;
}
static hmje_persist_record *hmje_persist_runtime(uint64_t id) {
    hmje_persist_init();
    for (uint32_t i = 0; i < hmje_persist_count; ++i)
        if (hmje_persist_records[i].runtime_id == id && id) return &hmje_persist_records[i];
    return NULL;
}
static void hmje_persist_close(uint64_t id, int quitting) {
    hmje_persist_record *record = hmje_persist_runtime(id);
    if (!record) return;
    record->runtime_id = 0;
    if (!quitting) record->active = 0;
    hmje_persist_save();
}
static void hmje_persist_frame(uint64_t id, int x, int y, int width, int height) {
    hmje_persist_record *record = hmje_persist_runtime(id);
    if (getenv("HMJE_WINDOW_PERSIST_DEBUG"))
        fprintf(stderr, "window-persist: frame id=%llu bound=%d %d,%d %dx%d\n",
                (unsigned long long)id, record != NULL, x, y, width, height);
    if (!record || width < 1 || height < 1) return;
    if (record->has_frame && record->x == x && record->y == y &&
        record->width == width && record->height == height) return;
    record->x = x; record->y = y; record->width = width; record->height = height;
    record->has_frame = 1;
    hmje_persist_save();
}
/* Returns the Nth active payload for a screen prefix, or -1. */
static int64_t hmje_persist_restore(const char *prefix, size_t prefix_len, uint32_t index,
                                    char *buffer, size_t capacity) {
    hmje_persist_init();
    for (uint32_t i = 0; i < hmje_persist_count; ++i) {
        hmje_persist_record *record = &hmje_persist_records[i];
        if (!record->active || record->payload_len == 0 || record->key_len < prefix_len ||
            memcmp(record->key, prefix, prefix_len) != 0) continue;
        if (index--) continue;
        if (capacity < record->payload_len) return -(int64_t)record->payload_len;
        memcpy(buffer, record->payload, record->payload_len);
        return (int64_t)record->payload_len;
    }
    return -1;
}
#endif
