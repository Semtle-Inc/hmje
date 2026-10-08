#define _GNU_SOURCE
#include "watch_common.h"
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int wd; char *path; } WatchDirectory;
typedef struct {
    WatchBase base;
    int fd, wake[2];
    pthread_t thread;
    WatchDirectory directories[8192];
    size_t directory_count;
} LinuxWatch;

static const char *watch_directory_path(LinuxWatch *watch, int wd) {
    for (size_t i = 0; i < watch->directory_count; ++i)
        if (watch->directories[i].wd == wd) return watch->directories[i].path;
    return NULL;
}
static void watch_add_tree(LinuxWatch *watch, const char *path) {
    if (watch->directory_count >= 8192 || watch_excluded(&watch->base, path)) return;
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return;
    int wd = inotify_add_watch(watch->fd, path, IN_CREATE | IN_MODIFY | IN_DELETE |
        IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF);
    if (wd < 0) return;
    watch->directories[watch->directory_count].wd = wd;
    watch->directories[watch->directory_count].path = strdup(path);
    if (!watch->directories[watch->directory_count].path) return;
    watch->directory_count++;
    DIR *directory = opendir(path);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.' &&
            (!entry->d_name[1] || (entry->d_name[1] == '.' && !entry->d_name[2]))) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < (int)sizeof(child))
            watch_add_tree(watch, child);
    }
    closedir(directory);
}
static void *watch_linux_worker(void *pointer) {
    LinuxWatch *watch = pointer;
    char buffer[65536];
    struct pollfd watched[2] = {{watch->fd, POLLIN, 0}, {watch->wake[0], POLLIN, 0}};
    while (poll(watched, 2, -1) >= 0) {
        if (watched[1].revents) break;
        if (!(watched[0].revents & POLLIN)) continue;
        ssize_t size;
        while ((size = read(watch->fd, buffer, sizeof(buffer))) > 0) {
            for (size_t offset = 0; offset + sizeof(struct inotify_event) <= (size_t)size;) {
                const struct inotify_event *event = (const void *)(buffer + offset);
                const char *parent = watch_directory_path(watch, event->wd);
                if (parent) {
                    char path[PATH_MAX];
                    int length = event->len ? snprintf(path, sizeof(path), "%s/%s", parent, event->name)
                                            : snprintf(path, sizeof(path), "%s", parent);
                    if (length > 0 && length < (int)sizeof(path)) {
                        uint64_t kind = (event->mask & (IN_MOVED_FROM | IN_MOVED_TO)) ? 4 :
                            (event->mask & (IN_DELETE | IN_DELETE_SELF)) ? 3 :
                            (event->mask & IN_CREATE) ? 1 : 2;
                        watch_push(&watch->base, path, kind);
                        if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)))
                            watch_add_tree(watch, path);
                    }
                }
                offset += sizeof(*event) + event->len;
            }
        }
        if (size < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    return NULL;
}
int64_t hmje_ui_watch_start(const uint8_t *root, uint64_t root_length,
                             const uint8_t *excludes, uint64_t exclude_length,
                             uint64_t delay_ms) {
    if (!root || !root_length || root[0] != '/' ||
        (exclude_length && !excludes) || delay_ms > 60000) return -EINVAL;
    char *root_text = watch_span(root, root_length);
    char *exclude_text = exclude_length ? watch_span(excludes, exclude_length) : NULL;
    if (!root_text || (exclude_length && !exclude_text)) {
        free(root_text); free(exclude_text); return -EINVAL;
    }
    struct stat info;
    if (stat(root_text, &info) != 0 || !S_ISDIR(info.st_mode)) {
        free(root_text); free(exclude_text); return -ENOENT;
    }
    LinuxWatch *watch = calloc(1, sizeof(*watch));
    if (!watch) { free(root_text); free(exclude_text); return -ENOMEM; }
    watch_base_init(&watch->base, root_text, exclude_text, delay_ms);
    watch->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch->fd < 0 || pipe(watch->wake) != 0) {
        if (watch->fd >= 0) close(watch->fd);
        watch_base_destroy(&watch->base); free(watch); return -errno;
    }
    watch_add_tree(watch, root_text);
    if (!watch->directory_count || pthread_create(&watch->thread, NULL, watch_linux_worker, watch)) {
        for (size_t i = 0; i < watch->directory_count; ++i) free(watch->directories[i].path);
        close(watch->fd); close(watch->wake[0]); close(watch->wake[1]);
        watch_base_destroy(&watch->base); free(watch); return -EIO;
    }
    return (int64_t)(uintptr_t)watch;
}
int64_t hmje_ui_watch_stop(int64_t handle) {
    if (handle <= 0) return -EINVAL;
    LinuxWatch *watch = (LinuxWatch *)(uintptr_t)handle;
    (void)write(watch->wake[1], "x", 1);
    pthread_join(watch->thread, NULL);
    close(watch->fd); close(watch->wake[0]); close(watch->wake[1]);
    for (size_t i = 0; i < watch->directory_count; ++i) free(watch->directories[i].path);
    watch_base_destroy(&watch->base); free(watch);
    return 0;
}
int64_t hmje_ui_watch_next(int64_t handle, uint8_t *output, uint64_t capacity, uint64_t *written) {
    return handle > 0 ? watch_next(&((LinuxWatch *)(uintptr_t)handle)->base, output, capacity, written) : -EINVAL;
}
void hmje_ui_watch_wait_async(int64_t handle, uint64_t token) {
    if (handle <= 0) { hmje_async_complete_u64(token, EINVAL, 0); return; }
    watch_wait_async(&((LinuxWatch *)(uintptr_t)handle)->base, token);
}
