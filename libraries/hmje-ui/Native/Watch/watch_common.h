#ifndef HMJE_UI_WATCH_COMMON_H
#define HMJE_UI_WATCH_COMMON_H
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);

typedef struct WatchEvent {
    struct WatchEvent *next;
    char *path;
    uint64_t kind;
} WatchEvent;
typedef struct WatchBase {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    WatchEvent *first, *last;
    size_t count;
    uint64_t delay_ms, last_ms;
    int stopped, waiter_active;
    char *root, *excludes;
} WatchBase;

static uint64_t watch_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static char *watch_span(const uint8_t *bytes, uint64_t length) {
    if (!bytes || !length || length > 131072 || memchr(bytes, 0, (size_t)length)) return NULL;
    char *copy = malloc((size_t)length + 1);
    if (!copy) return NULL;
    memcpy(copy, bytes, (size_t)length);
    copy[length] = 0;
    return copy;
}
static void watch_base_init(WatchBase *watch, char *root, char *excludes, uint64_t delay_ms) {
    pthread_mutex_init(&watch->lock, NULL);
    pthread_cond_init(&watch->changed, NULL);
    watch->root = root;
    watch->excludes = excludes;
    watch->delay_ms = delay_ms;
}
static int watch_excluded(WatchBase *watch, const char *path) {
    const char *relative = path;
    size_t root_length = strlen(watch->root);
    if (strncmp(path, watch->root, root_length) == 0 && path[root_length] == '/') relative += root_length + 1;
    if (!watch->excludes) return 0;
    const char *cursor = watch->excludes;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length && length < 4096) {
            char pattern[4096];
            memcpy(pattern, cursor, length);
            pattern[length] = 0;
            if (fnmatch(pattern, relative, 0) == 0) return 1;
            const char *part = relative;
            while (*part) {
                const char *slash = strchr(part, '/');
                size_t part_length = slash ? (size_t)(slash - part) : strlen(part);
                char component[4096];
                if (part_length < sizeof(component)) {
                    memcpy(component, part, part_length); component[part_length] = 0;
                    if (fnmatch(pattern, component, 0) == 0) return 1;
                }
                if (!slash) break;
                part = slash + 1;
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
    return 0;
}
static void watch_push(WatchBase *watch, const char *path, uint64_t kind) {
    if (!path || watch_excluded(watch, path)) return;
    WatchEvent *event = calloc(1, sizeof(*event));
    if (!event) return;
    event->path = strdup(path);
    if (!event->path) { free(event); return; }
    event->kind = kind;
    pthread_mutex_lock(&watch->lock);
    if (watch->stopped || watch->count >= 4096) {
        pthread_mutex_unlock(&watch->lock);
        free(event->path); free(event); return;
    }
    if (watch->last) watch->last->next = event;
    else watch->first = event;
    watch->last = event;
    watch->count++;
    watch->last_ms = watch_now_ms();
    pthread_cond_broadcast(&watch->changed);
    pthread_mutex_unlock(&watch->lock);
}
static void *watch_wait_worker(void *pointer) {
    struct { WatchBase *watch; uint64_t token; } *request = pointer;
    WatchBase *watch = request->watch;
    uint64_t token = request->token;
    free(request);
    pthread_mutex_lock(&watch->lock);
    while (!watch->stopped) {
        uint64_t now = watch_now_ms();
        if (watch->first && now >= watch->last_ms + watch->delay_ms) break;
        if (!watch->first) pthread_cond_wait(&watch->changed, &watch->lock);
        else {
            uint64_t remaining = watch->last_ms + watch->delay_ms - now;
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += (time_t)(remaining / 1000);
            deadline.tv_nsec += (long)(remaining % 1000) * 1000000;
            if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&watch->changed, &watch->lock, &deadline);
        }
    }
    int stopped = watch->stopped;
    pthread_mutex_unlock(&watch->lock);
    hmje_async_complete_u64(token, 0, stopped ? 0 : 1);
    pthread_mutex_lock(&watch->lock);
    watch->waiter_active = 0;
    pthread_cond_broadcast(&watch->changed);
    pthread_mutex_unlock(&watch->lock);
    return NULL;
}
static void watch_wait_async(WatchBase *watch, uint64_t token) {
    pthread_mutex_lock(&watch->lock);
    if (watch->stopped || watch->waiter_active) {
        pthread_mutex_unlock(&watch->lock);
        hmje_async_complete_u64(token, EBUSY, 0);
        return;
    }
    watch->waiter_active = 1;
    pthread_mutex_unlock(&watch->lock);
    struct { WatchBase *watch; uint64_t token; } *request = malloc(sizeof(*request));
    if (!request) {
        pthread_mutex_lock(&watch->lock); watch->waiter_active = 0; pthread_mutex_unlock(&watch->lock);
        hmje_async_complete_u64(token, ENOMEM, 0); return;
    }
    request->watch = watch; request->token = token;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, watch_wait_worker, request);
    if (error) {
        free(request);
        pthread_mutex_lock(&watch->lock); watch->waiter_active = 0; pthread_mutex_unlock(&watch->lock);
        hmje_async_complete_u64(token, error, 0);
    } else pthread_detach(thread);
}
static int64_t watch_next(WatchBase *watch, uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!watch || !output || !written || capacity < 8) return -EINVAL;
    pthread_mutex_lock(&watch->lock);
    if (watch->stopped) { pthread_mutex_unlock(&watch->lock); return -ECANCELED; }
    if (!watch->first || watch_now_ms() < watch->last_ms + watch->delay_ms) {
        pthread_mutex_unlock(&watch->lock); return 0;
    }
    size_t size = 8;
    for (WatchEvent *event = watch->first; event; event = event->next) {
        size_t length = strlen(event->path);
        if (length > SIZE_MAX - size - 16) { pthread_mutex_unlock(&watch->lock); return -EOVERFLOW; }
        size += 16 + length;
    }
    if (size > capacity) { pthread_mutex_unlock(&watch->lock); return -ERANGE; }
    uint64_t count = watch->count;
    memcpy(output, &count, 8);
    size_t offset = 8;
    WatchEvent *event = watch->first;
    while (event) {
        size_t length = strlen(event->path);
        uint64_t bytes = length;
        memcpy(output + offset, &event->kind, 8); offset += 8;
        memcpy(output + offset, &bytes, 8); offset += 8;
        memcpy(output + offset, event->path, length); offset += length;
        WatchEvent *next = event->next;
        free(event->path); free(event); event = next;
    }
    watch->first = watch->last = NULL; watch->count = 0;
    *written = offset;
    pthread_mutex_unlock(&watch->lock);
    return 1;
}
static void watch_base_destroy(WatchBase *watch) {
    pthread_mutex_lock(&watch->lock);
    watch->stopped = 1;
    pthread_cond_broadcast(&watch->changed);
    while (watch->waiter_active) pthread_cond_wait(&watch->changed, &watch->lock);
    WatchEvent *event = watch->first;
    while (event) { WatchEvent *next = event->next; free(event->path); free(event); event = next; }
    pthread_mutex_unlock(&watch->lock);
    pthread_cond_destroy(&watch->changed);
    pthread_mutex_destroy(&watch->lock);
    free(watch->root); free(watch->excludes);
}
#endif
