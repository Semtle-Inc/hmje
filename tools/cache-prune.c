#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Cache entries are immutable and every reader treats a missing entry as a miss.
 * Never follow links, enter an active work directory, or delete a recent entry.
 * Concurrent pruners are serialized; concurrent compilers may simply rebuild an
 * entry removed between lookup and copy. */
struct entry { char *path; uint64_t size; time_t used; dev_t device; ino_t inode; int directory; };
struct scan {
    struct entry *items;
    size_t count, capacity;
    uint64_t total;
    time_t now, min_age;
    dev_t device;
    int failed;
};

static char *join(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    if (!a) return strdup(name);
    if (a > SIZE_MAX - b - 2) return NULL;
    char *path = malloc(a + b + 2);
    if (!path) return NULL;
    memcpy(path, dir, a);
    path[a] = '/';
    memcpy(path + a + 1, name, b + 1);
    return path;
}

static int scratch_name(const char *name) {
    return !strncmp(name, "work-", 5) || !strncmp(name, "invocation.", 11)
        || !strncmp(name, ".tmp", 4) || strstr(name, ".tmp.") != NULL
        || !strcmp(name, ".publish.lock");
}

/* A product work directory contains the coordinator PID in its generated name.
 * Only a dead owner may become a GC candidate; unknown names stay untouched. */
static int dead_work_owner(const char *name) {
    if (strncmp(name, "work-", 5)) return 0;
    const char *first = strchr(name + 5, '-');
    if (!first || first == name + 5) return 0;
    for (const char *p = name + 5; p < first; ++p) if (*p < '0' || *p > '9') return 0;
    const char *second = strchr(first + 1, '-');
    if (!second || second == first + 1 || !second[1]) return 0;
    for (const char *p = first + 1; p < second; ++p) if (*p < '0' || *p > '9') return 0;
    for (const char *p = second + 1; *p; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    unsigned long long raw = strtoull(first + 1, NULL, 10);
    if (errno || raw == 0 || raw > INT32_MAX) return 0;
    pid_t owner = (pid_t)raw;
    if (kill(owner, 0) == 0 || errno == EPERM) return 0;
    return errno == ESRCH;
}

/* New invocation directories carry the shell driver's PID. Older random-only
 * names have no safe liveness proof and intentionally remain untouched. */
static int dead_invocation_owner(const char *name) {
    if (strncmp(name, "invocation.", 11)) return 0;
    const char *start = name + 11;
    const char *end = strchr(start, '.');
    if (!end || end == start || !end[1]) return 0;
    for (const char *p = start; p < end; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    unsigned long long raw = strtoull(start, NULL, 10);
    if (errno || raw == 0 || raw > INT32_MAX) return 0;
    pid_t owner = (pid_t)raw;
    if (kill(owner, 0) == 0 || errno == EPERM) return 0;
    return errno == ESRCH;
}

/* Inspect a private work tree without following links or crossing devices.
 * A recent descendant protects the entire tree, even if the directory's own
 * mtime is old. This is rechecked immediately before deletion. */
static int measure_tree(int dir_fd, dev_t device, unsigned depth,
                        uint64_t *bytes, time_t *newest) {
    if (depth > 24) return -1;
    int copy_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (copy_fd < 0) return -1;
    DIR *stream = fdopendir(copy_fd);
    if (!stream) { close(copy_fd); return -1; }
    struct dirent *item;
    int result = 0;
    errno = 0;
    while ((item = readdir(stream))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dir_fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) result = -1;
            continue;
        }
        if (st.st_dev != device) { result = -1; break; }
        if (st.st_mtime > *newest) *newest = st.st_mtime;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dir_fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0 || measure_tree(child, device, depth + 1, bytes, newest)) result = -1;
            if (child >= 0) close(child);
        } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
            uint64_t size = (uint64_t)st.st_size;
            *bytes = UINT64_MAX - *bytes < size ? UINT64_MAX : *bytes + size;
        }
        if (result) break;
        errno = 0;
    }
    if (errno && errno != ENOENT) result = -1;
    if (closedir(stream)) result = -1;
    return result;
}

static int erase_tree(int dir_fd, dev_t device, unsigned depth) {
    if (depth > 24) return -1;
    int copy_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (copy_fd < 0) return -1;
    DIR *stream = fdopendir(copy_fd);
    if (!stream) { close(copy_fd); return -1; }
    struct dirent *item;
    int result = 0;
    errno = 0;
    while ((item = readdir(stream))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dir_fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) result = -1;
            continue;
        }
        if (st.st_dev != device) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dir_fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0 || erase_tree(child, device, depth + 1)) result = -1;
            if (child >= 0) close(child);
            if (!result && unlinkat(dir_fd, item->d_name, AT_REMOVEDIR) && errno != ENOENT) result = -1;
        } else if (unlinkat(dir_fd, item->d_name, 0) && errno != ENOENT) result = -1;
        if (result) break;
        errno = 0;
    }
    if (errno && errno != ENOENT) result = -1;
    if (closedir(stream)) result = -1;
    return result;
}

static void scan_dir(struct scan *scan, int dir_fd, const char *relative, unsigned depth) {
    if (depth > 24) { scan->failed = 1; return; }
    int copy_fd = dup(dir_fd);
    if (copy_fd < 0) { scan->failed = 1; return; }
    DIR *stream = fdopendir(copy_fd);
    if (!stream) {
        close(copy_fd);
        scan->failed = 1;
        return;
    }
    struct dirent *item;
    errno = 0;
    while ((item = readdir(stream))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (!*relative && strncmp(item->d_name, "invocation.", 11)) continue;
        char *path = join(relative, item->d_name);
        struct stat st;
        if (!path) { scan->failed = 1; break; }
        if (fstatat(dir_fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) scan->failed = 1;
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (((!strcmp(relative, "products") && dead_work_owner(item->d_name))
                    || (!*relative && dead_invocation_owner(item->d_name))
                    || (*relative && strstr(item->d_name, ".stale-") != NULL))
                && st.st_dev == scan->device) {
                int child = openat(dir_fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                uint64_t size = 0;
                time_t newest = st.st_mtime;
                /* rename preserves the old directory mtime. A just-quarantined
                 * cache entry must still receive the normal reader grace period. */
                if (strstr(item->d_name, ".stale-") != NULL && st.st_ctime > newest)
                    newest = st.st_ctime;
                if (child >= 0) {
                    int measured = measure_tree(child, scan->device, 0, &size, &newest);
                    close(child);
                    if (!measured) {
                        scan->total = UINT64_MAX - scan->total < size ? UINT64_MAX : scan->total + size;
                        if (newest <= scan->now && scan->now - newest >= scan->min_age) {
                            if (scan->count == scan->capacity) {
                                size_t next = scan->capacity ? scan->capacity * 2 : 1024;
                                if (next < scan->capacity || next > SIZE_MAX / sizeof(*scan->items)) scan->failed = 1;
                                else {
                                    struct entry *grown = realloc(scan->items, next * sizeof(*scan->items));
                                    if (!grown) scan->failed = 1;
                                    else { scan->items = grown; scan->capacity = next; }
                                }
                            }
                            if (!scan->failed) {
                                scan->items[scan->count++] = (struct entry){path, size, newest, st.st_dev, st.st_ino, 1};
                                path = NULL;
                            }
                        }
                    }
                }
            } else if (*relative && !scratch_name(item->d_name)) {
                int child = openat(dir_fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                if (child >= 0) {
                    scan_dir(scan, child, path, depth + 1);
                    close(child);
                } else if (errno != ENOENT && errno != ELOOP) scan->failed = 1;
            }
        } else if (*relative && S_ISREG(st.st_mode)) {
            uint64_t size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
            scan->total = UINT64_MAX - scan->total < size ? UINT64_MAX : scan->total + size;
            time_t used = st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime;
            if (!scratch_name(item->d_name) && used <= scan->now && scan->now - used >= scan->min_age) {
                if (scan->count == scan->capacity) {
                    size_t next = scan->capacity ? scan->capacity * 2 : 1024;
                    if (next < scan->capacity || next > SIZE_MAX / sizeof(*scan->items)) {
                        scan->failed = 1;
                    } else {
                        struct entry *grown = realloc(scan->items, next * sizeof(*scan->items));
                        if (!grown) scan->failed = 1;
                        else { scan->items = grown; scan->capacity = next; }
                    }
                }
                if (!scan->failed) {
                    scan->items[scan->count++] = (struct entry){path, size, used, st.st_dev, st.st_ino, 0};
                    path = NULL;
                }
            }
        }
        free(path);
        if (scan->failed) break;
        errno = 0;
    }
    if (errno && errno != ENOENT) scan->failed = 1;
    if (closedir(stream) != 0) scan->failed = 1;
}

/* Re-open every directory component below the pinned root without following a
 * link. A cache directory may be replaced while a prune is running; a string
 * path followed at deletion time would otherwise escape the selected root. */
static int open_parent_at(int root_fd, const char *relative, char **name_out) {
    char *copy = strdup(relative);
    if (!copy) return -1;
    int parent = dup(root_fd);
    if (parent < 0) { free(copy); return -1; }
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (!slash) break;
        *slash = '\0';
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
            close(parent); free(copy); return -1;
        }
        int child = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(parent);
        if (child < 0) { free(copy); return -1; }
        parent = child;
        part = slash + 1;
    }
    if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
        close(parent); free(copy); return -1;
    }
    *name_out = strdup(part);
    free(copy);
    if (!*name_out) { close(parent); return -1; }
    return parent;
}

static int oldest_first(const void *left, const void *right) {
    const struct entry *a = left, *b = right;
    /* Once eligible, superseded repair entries are cheaper to discard than
     * still-live cache keys, even when both have the same coarse timestamp. */
    int a_stale = strstr(a->path, ".stale-") != NULL;
    int b_stale = strstr(b->path, ".stale-") != NULL;
    if (a_stale != b_stale) return a_stale ? -1 : 1;
    if (a->used != b->used) return a->used < b->used ? -1 : 1;
    return strcmp(a->path, b->path);
}

static int uint_arg(const char *value, uint64_t *out) {
    if (!*value || *value == '-') return -1;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || *end) return -1;
    *out = (uint64_t)parsed;
    return 0;
}

/* Policy knobs come from the environment so every driver (installed, checkout,
 * build-native output, stage0) shares one policy without new flags. A malformed
 * value falls back to the default: automatic maintenance must never fail a build. */
static uint64_t env_uint(const char *name, uint64_t fallback, uint64_t low, uint64_t high) {
    const char *value = getenv(name);
    uint64_t parsed;
    if (!value || uint_arg(value, &parsed) || parsed < low || parsed > high) return fallback;
    return parsed;
}

#define GIB (1024ULL * 1024 * 1024)
#define MAX_GIB_LIMIT (UINT64_MAX / GIB)
#define MAX_MINUTES (INT64_MAX / 60 / 2)

/* Free space of the cache volume. HMJE_CACHE_FREE_BYTES replaces the measured
 * value so tests can simulate a full disk without filling one. */
static int volume_space(int root_fd, uint64_t *free_bytes, uint64_t *total_bytes) {
    struct statvfs vfs;
    *free_bytes = UINT64_MAX;
    *total_bytes = 0;
    if (!fstatvfs(root_fd, &vfs)) {
        uint64_t unit = vfs.f_frsize ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
        uint64_t avail = (uint64_t)vfs.f_bavail, blocks = (uint64_t)vfs.f_blocks;
        *free_bytes = unit && avail > UINT64_MAX / unit ? UINT64_MAX : avail * unit;
        *total_bytes = unit && blocks > UINT64_MAX / unit ? UINT64_MAX : blocks * unit;
    }
    const char *override = getenv("HMJE_CACHE_FREE_BYTES");
    uint64_t forced;
    if (override && !uint_arg(override, &forced)) *free_bytes = forced;
    return *free_bytes != UINT64_MAX;
}

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "prune")) {
        fprintf(stderr, "usage: hmjec-cache prune ROOT [--max-gib N] [--min-age-hours N|--min-age-minutes N] [--automatic] [--dry-run]\n");
        return 2;
    }
    /* Normal policy: 10 GiB, entries idle for an hour, checked at most hourly.
     * Low-space policy (free < max(15 GiB, 5 % of the volume)): 4 GiB, entries
     * idle for 10 minutes, checked every 5 minutes. Explicit flags replace the
     * normal values; low-space values never loosen them. */
    uint64_t max_gib = env_uint("HMJE_CACHE_MAX_GIB", 10, 1, MAX_GIB_LIMIT);
    uint64_t min_minutes = env_uint("HMJE_CACHE_MIN_AGE_MINUTES", 60, 0, MAX_MINUTES);
    uint64_t interval_minutes = env_uint("HMJE_CACHE_GC_INTERVAL_MINUTES", 60, 0, MAX_MINUTES);
    uint64_t low_free_gib = env_uint("HMJE_CACHE_LOW_FREE_GIB", 15, 0, MAX_GIB_LIMIT);
    uint64_t low_free_percent = env_uint("HMJE_CACHE_LOW_FREE_PERCENT", 5, 0, 100);
    uint64_t low_max_gib = env_uint("HMJE_CACHE_LOW_MAX_GIB", 4, 1, MAX_GIB_LIMIT);
    uint64_t low_min_minutes = env_uint("HMJE_CACHE_LOW_MIN_AGE_MINUTES", 10, 0, MAX_MINUTES);
    uint64_t low_interval_minutes = env_uint("HMJE_CACHE_LOW_INTERVAL_MINUTES", 5, 0, MAX_MINUTES);
    int automatic = 0, dry_run = 0;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--automatic")) automatic = 1;
        else if (!strcmp(argv[i], "--dry-run")) dry_run = 1;
        else if ((!strcmp(argv[i], "--max-gib") || !strcmp(argv[i], "--min-age-hours")
                  || !strcmp(argv[i], "--min-age-minutes")) && i + 1 < argc) {
            uint64_t value;
            if (uint_arg(argv[i + 1], &value)) return 2;
            if (!strcmp(argv[i], "--max-gib")) max_gib = value;
            else if (!strcmp(argv[i], "--min-age-hours")) {
                if (value > MAX_MINUTES / 60) return 2;
                min_minutes = value * 60;
            } else min_minutes = value;
            ++i;
        } else return 2;
    }
    if (!max_gib || max_gib > MAX_GIB_LIMIT || min_minutes > MAX_MINUTES) return 2;
    int root_fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root_fd < 0) return 2;
    struct stat root_st;
    if (fstat(root_fd, &root_st)) { close(root_fd); return 2; }
    dev_t root_device = root_st.st_dev;
    time_t now = time(NULL);
    time_t since_last = -1;
    struct stat stamp_st;
    if (!fstatat(root_fd, ".gc-last", &stamp_st, AT_SYMLINK_NOFOLLOW)
        && S_ISREG(stamp_st.st_mode) && now >= stamp_st.st_mtime)
        since_last = now - stamp_st.st_mtime;
    uint64_t low_interval = low_interval_minutes < interval_minutes ? low_interval_minutes : interval_minutes;
    if (automatic && since_last >= 0 && (uint64_t)since_last < low_interval * 60) {
        close(root_fd); return 0;
    }
    uint64_t free_bytes, total_bytes;
    int measured = volume_space(root_fd, &free_bytes, &total_bytes);
    uint64_t low_threshold = low_free_gib * GIB;
    if (total_bytes / 100 * low_free_percent > low_threshold) low_threshold = total_bytes / 100 * low_free_percent;
    int low_space = measured && free_bytes < low_threshold;
    if (automatic && !low_space && since_last >= 0 && (uint64_t)since_last < interval_minutes * 60) {
        close(root_fd); return 0;
    }
    if (low_space) {
        if (low_max_gib < max_gib) max_gib = low_max_gib;
        if (low_min_minutes < min_minutes) min_minutes = low_min_minutes;
    }
    int lock_fd = openat(root_fd, ".gc-lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        close(root_fd);
        return 1;
    }
    struct flock guard = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(lock_fd, F_SETLK, &guard)) {
        close(lock_fd); close(root_fd);
        return errno == EACCES || errno == EAGAIN ? 0 : 1;
    }
    struct scan scan = {.now = now, .min_age = (time_t)(min_minutes * 60), .device = root_device};
    int parse_fd = openat(root_fd, "parse", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int parse_error = errno;
    int products_fd = openat(root_fd, "products", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int products_error = errno;
    if (parse_fd >= 0) { scan_dir(&scan, parse_fd, "parse", 0); close(parse_fd); }
    else if (parse_error != ENOENT) scan.failed = 1;
    if (products_fd >= 0) { scan_dir(&scan, products_fd, "products", 0); close(products_fd); }
    else if (products_error != ENOENT) scan.failed = 1;
    scan_dir(&scan, root_fd, "", 0);
    uint64_t limit = max_gib * GIB;
    uint64_t before = scan.total, removed = 0;
    if (!scan.failed && scan.total > limit) {
        qsort(scan.items, scan.count, sizeof(*scan.items), oldest_first);
        for (size_t i = 0; i < scan.count && scan.total > limit; ++i) {
            char *name = NULL;
            int parent = open_parent_at(root_fd, scan.items[i].path, &name);
            if (parent < 0) continue;
            struct stat current;
            if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW)
                || current.st_dev != scan.items[i].device || current.st_ino != scan.items[i].inode
                || (scan.items[i].directory ? !S_ISDIR(current.st_mode) : !S_ISREG(current.st_mode))) {
                close(parent); free(name); continue;
            }
            if (scan.items[i].directory) {
                if (!(dead_work_owner(name) || dead_invocation_owner(name)
                      || strstr(name, ".stale-") != NULL)) {
                    close(parent); free(name); continue;
                }
                int child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                uint64_t size = 0;
                time_t newest = current.st_mtime;
                if (strstr(name, ".stale-") != NULL && current.st_ctime > newest)
                    newest = current.st_ctime;
                int valid = child >= 0 && !measure_tree(child, scan.device, 0, &size, &newest)
                    && size == scan.items[i].size && newest == scan.items[i].used
                    && newest <= scan.now && scan.now - newest >= scan.min_age;
                if (valid && (dry_run || (!erase_tree(child, scan.device, 0)
                    && !unlinkat(parent, name, AT_REMOVEDIR)))) {
                    scan.total -= size;
                    ++removed;
                }
                if (child >= 0) close(child);
                close(parent); free(name); continue;
            }
            time_t used = current.st_atime > current.st_mtime ? current.st_atime : current.st_mtime;
            if (used != scan.items[i].used || current.st_size < 0 || (uint64_t)current.st_size != scan.items[i].size) {
                close(parent); free(name); continue;
            }
            if (dry_run || !unlinkat(parent, name, 0)) {
                scan.total -= scan.items[i].size;
                ++removed;
            }
            close(parent); free(name);
        }
    }
    if (!scan.failed && !dry_run) {
        int stamp_fd = openat(root_fd, ".gc-last", O_CREAT | O_WRONLY | O_NOFOLLOW, 0600);
        if (stamp_fd >= 0) close(stamp_fd);
        struct timespec times[2] = {{now, 0}, {now, 0}};
        if (stamp_fd >= 0) utimensat(root_fd, ".gc-last", times, AT_SYMLINK_NOFOLLOW);
    }
    if (!automatic) {
        printf("hmjec-cache: before=%" PRIu64 " after=%" PRIu64 " removed=%" PRIu64
               " mode=%s budget=%" PRIu64 " min-age-minutes=%" PRIu64,
               before, scan.total, removed, low_space ? "low-space" : "normal", limit, min_minutes);
        if (measured) printf(" free=%" PRIu64, free_bytes);
        printf("%s%s\n", dry_run ? " dry-run" : "", scan.failed ? " scan-failed" : "");
    }
    for (size_t i = 0; i < scan.count; ++i) free(scan.items[i].path);
    free(scan.items);
    close(lock_fd); close(root_fd);
    return scan.failed ? 1 : 0;
}
