/* Existing compiler-host POSIX services, separated from the application shim.
 * Source discovery, tool fingerprints and compiler working files are only
 * linked into compiler tools. Their implementation is unchanged here. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#endif

/*
 * Canonical path identity for the self-hosted module loader.  HMJE strings are
 * byte spans rather than NUL-terminated C strings, so copy the input and let
 * realpath allocate its result before checking the caller's output capacity.
 */
int64_t hmje_posix_realpath(
    uint64_t input_address,
    uint64_t input_length,
    uint64_t output_address,
    uint64_t output_capacity
) {
    if (input_address == 0 || input_length == 0 || input_length >= 65536 ||
        output_address == 0 || output_capacity == 0) {
        return -1;
    }
    char *input = (char *)malloc((size_t)input_length + 1);
    if (input == NULL) return -1;
    memcpy(input, (const void *)(uintptr_t)input_address, (size_t)input_length);
    input[input_length] = '\0';
    char *resolved = realpath(input, NULL);
    free(input);
    if (resolved == NULL) return -1;
    size_t length = strlen(resolved);
    if (length >= output_capacity) {
        free(resolved);
        return -1;
    }
    memcpy((void *)(uintptr_t)output_address, resolved, length);
    free(resolved);
    return (int64_t)length;
}

/*
 * Source paths the compiler compares must have one spelling. macOS file
 * systems resolve NFC and NFD names to the same file, but realpath/readdir
 * return the on-disk bytes: an app bundle copied from a disk image or by
 * Finder can store Hangul names decomposed (NFD) while manifests and module
 * maps spell them composed (NFC). Without normalization, the same library file can reach the
 * loader under two byte strings and be parsed twice.
 * Compiler-host paths are therefore returned in NFC. Returns a malloc'd copy.
 */
static char *hmje_host_path_nfc(const char *path) {
#if defined(__APPLE__)
    int ascii = 1;
    for (const unsigned char *cursor = (const unsigned char *)path; *cursor; ++cursor) {
        if (*cursor >= 0x80) { ascii = 0; break; }
    }
    if (!ascii) {
        CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
        if (text != NULL) {
            CFMutableStringRef composed = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, text);
            CFRelease(text);
            if (composed != NULL) {
                CFStringNormalize(composed, kCFStringNormalizationFormC);
                CFIndex capacity = CFStringGetMaximumSizeForEncoding(
                    CFStringGetLength(composed), kCFStringEncodingUTF8) + 1;
                char *result = (char *)malloc((size_t)capacity);
                if (result != NULL &&
                    CFStringGetCString(composed, result, capacity, kCFStringEncodingUTF8)) {
                    CFRelease(composed);
                    return result;
                }
                free(result);
                CFRelease(composed);
            }
        }
    }
#endif
    return strdup(path);
}

/* Compiler-host ABI. Keep application file syscalls distinct from the
 * compiler's unrestricted project-source discovery capability. */
int64_t hmje_host_realpath(uint64_t input_address, uint64_t input_length,
                           uint64_t output_address, uint64_t output_capacity) {
    if (input_address == 0 || input_length == 0 || input_length >= 65536 ||
        output_address == 0 || output_capacity == 0) {
        return -1;
    }
    char *input = (char *)malloc((size_t)input_length + 1);
    if (input == NULL) return -1;
    memcpy(input, (const void *)(uintptr_t)input_address, (size_t)input_length);
    input[input_length] = '\0';
    char *resolved = realpath(input, NULL);
    free(input);
    if (resolved == NULL) return -1;
    char *composed = hmje_host_path_nfc(resolved);
    free(resolved);
    if (composed == NULL) return -1;
    size_t length = strlen(composed);
    if (length >= output_capacity) {
        free(composed);
        return -1;
    }
    memcpy((void *)(uintptr_t)output_address, composed, length);
    free(composed);
    return (int64_t)length;
}

typedef struct hmje_source_paths {
    char **items;
    size_t count;
    size_t capacity;
} hmje_source_paths;

static void hmje_source_paths_destroy(hmje_source_paths *paths) {
    for (size_t index = 0; index < paths->count; ++index) free(paths->items[index]);
    free(paths->items);
}

static int hmje_source_paths_add(hmje_source_paths *paths, const char *path) {
    if (paths->count == paths->capacity) {
        size_t capacity = paths->capacity == 0 ? 16 : paths->capacity * 2;
        if (capacity < paths->capacity || capacity > SIZE_MAX / sizeof(char *)) {
            return EOVERFLOW;
        }
        char **items = (char **)realloc(paths->items, capacity * sizeof(char *));
        if (items == NULL) return ENOMEM;
        paths->items = items;
        paths->capacity = capacity;
    }
    paths->items[paths->count] = hmje_host_path_nfc(path);
    if (paths->items[paths->count] == NULL) return ENOMEM;
    paths->count += 1;
    return 0;
}

static int hmje_path_has_source_suffix(const char *path) {
    size_t length = strlen(path);
    return length >= 5 && strcmp(path + length - 5, ".hmje") == 0;
}

static int hmje_collect_paths(
    const char *directory,
    hmje_source_paths *paths,
    int source_only
) {
    DIR *stream = opendir(directory);
    if (stream == NULL) return errno;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (entry == NULL) {
            if (errno != 0) error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t directory_length = strlen(directory);
        size_t name_length = strlen(entry->d_name);
        if (directory_length > SIZE_MAX - name_length - 2) {
            error = EOVERFLOW;
            break;
        }
        char *path = (char *)malloc(directory_length + name_length + 2);
        if (path == NULL) {
            error = ENOMEM;
            break;
        }
        memcpy(path, directory, directory_length);
        path[directory_length] = '/';
        memcpy(path + directory_length + 1, entry->d_name, name_length + 1);
        struct stat metadata;
        if (lstat(path, &metadata) != 0) {
            error = errno;
        } else if (S_ISLNK(metadata.st_mode)) {
            error = ELOOP;
        } else if (S_ISDIR(metadata.st_mode)) {
            error = hmje_collect_paths(path, paths, source_only);
        } else if (S_ISREG(metadata.st_mode) &&
                   (!source_only || hmje_path_has_source_suffix(path))) {
            error = hmje_source_paths_add(paths, path);
        }
        free(path);
        if (error != 0) break;
    }
    if (closedir(stream) != 0 && error == 0) error = errno;
    return error;
}

static int hmje_compare_source_paths(const void *left, const void *right) {
    const char *const *left_path = (const char *const *)left;
    const char *const *right_path = (const char *const *)right;
    return strcmp(*left_path, *right_path);
}

/*
 * Deterministic module source discovery for the HMJE-native project planner.
 * A zero output address is a size query. The second call receives newline-
 * separated canonical paths. Symlinks are rejected so recursive discovery
 * cannot silently leave the declared module source boundary.
 */
static int64_t hmje_posix_list_project_paths(
    uint64_t input_address,
    uint64_t input_length,
    uint64_t output_address,
    uint64_t output_capacity,
    int source_only
) {
    if (input_address == 0 || input_length == 0 || input_length >= 65536) return -EINVAL;
    char *input = (char *)malloc((size_t)input_length + 1);
    if (input == NULL) return -ENOMEM;
    memcpy(input, (const void *)(uintptr_t)input_address, (size_t)input_length);
    input[input_length] = '\0';
    if (memchr(input, '\0', (size_t)input_length) != NULL) {
        free(input);
        return -EINVAL;
    }
    char *root = realpath(input, NULL);
    free(input);
    if (root == NULL) return -(int64_t)errno;

    hmje_source_paths paths = {0};
    struct stat metadata;
    int error = 0;
    if (lstat(root, &metadata) != 0) {
        error = errno;
    } else if (S_ISDIR(metadata.st_mode)) {
        error = hmje_collect_paths(root, &paths, source_only);
    } else if (S_ISREG(metadata.st_mode) &&
               (!source_only || hmje_path_has_source_suffix(root))) {
        error = hmje_source_paths_add(&paths, root);
    } else {
        error = ENOTDIR;
    }
    free(root);
    if (error != 0) {
        hmje_source_paths_destroy(&paths);
        return -(int64_t)error;
    }
    qsort(paths.items, paths.count, sizeof(char *), hmje_compare_source_paths);

    size_t required = 0;
    for (size_t index = 0; index < paths.count; ++index) {
        size_t length = strlen(paths.items[index]);
        if (required > SIZE_MAX - length - 1) {
            hmje_source_paths_destroy(&paths);
            return -EOVERFLOW;
        }
        required += length + 1;
    }
    if (required > INT64_MAX) {
        hmje_source_paths_destroy(&paths);
        return -EOVERFLOW;
    }
    if (output_address == 0) {
        hmje_source_paths_destroy(&paths);
        return (int64_t)required;
    }
    if (output_capacity < required) {
        hmje_source_paths_destroy(&paths);
        return -ENOSPC;
    }
    unsigned char *output = (unsigned char *)(uintptr_t)output_address;
    size_t offset = 0;
    for (size_t index = 0; index < paths.count; ++index) {
        size_t length = strlen(paths.items[index]);
        memcpy(output + offset, paths.items[index], length);
        offset += length;
        output[offset++] = '\n';
    }
    hmje_source_paths_destroy(&paths);
    return (int64_t)required;
}

int64_t hmje_posix_list_hmje_sources(
    uint64_t input_address,
    uint64_t input_length,
    uint64_t output_address,
    uint64_t output_capacity
) {
    return hmje_posix_list_project_paths(
        input_address, input_length, output_address, output_capacity, 1
    );
}

int64_t hmje_posix_list_project_files(
    uint64_t input_address,
    uint64_t input_length,
    uint64_t output_address,
    uint64_t output_capacity
) {
    return hmje_posix_list_project_paths(
        input_address, input_length, output_address, output_capacity, 0
    );
}

int64_t hmje_host_list_hmje_sources(uint64_t input_address, uint64_t input_length,
                                    uint64_t output_address, uint64_t output_capacity) {
    return hmje_posix_list_hmje_sources(input_address, input_length,
                                        output_address, output_capacity);
}

int64_t hmje_host_list_project_files(uint64_t input_address, uint64_t input_length,
                                     uint64_t output_address, uint64_t output_capacity) {
    return hmje_posix_list_project_files(input_address, input_length,
                                         output_address, output_capacity);
}

static char *hmje_posix_copy_path(uint64_t address, uint64_t length);

static int hmje_remove_work_tree(const char *path, unsigned depth) {
    if (depth > 256) return -ELOOP;
    struct stat info;
    if (lstat(path, &info)) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(info.st_mode)) return -EINVAL;
    DIR *directory = opendir(path);
    if (!directory) return -errno;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t a = strlen(path), b = strlen(entry->d_name);
        if (a > SIZE_MAX - b - 2) { result = -ENOMEM; break; }
        char *child = malloc(a + b + 2);
        if (!child) { result = -ENOMEM; break; }
        memcpy(child, path, a);
        child[a] = '/';
        memcpy(child + a + 1, entry->d_name, b + 1);
        if (lstat(child, &info)) result = -errno;
        else if (S_ISDIR(info.st_mode)) result = hmje_remove_work_tree(child, depth + 1);
        else if (unlink(child)) result = -errno;
        free(child);
        if (result) break;
    }
    if (closedir(directory) && !result) result = -errno;
    if (!result && rmdir(path)) result = -errno;
    return result;
}

int64_t hmje_host_directory_remove_tree(uint64_t path_address, uint64_t path_length) {
    char *path = hmje_posix_copy_path(path_address, path_length);
    if (!path) return -EINVAL;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    int64_t result = strncmp(name, "work-", 5) ? -EINVAL
                     : hmje_remove_work_tree(path, 0);
    free(path);
    return result;
}

uint64_t hmje_posix_path_slot_hash(uint64_t path_address, uint64_t path_length) {
    if (path_address == 0) return 0;
    const unsigned char *bytes = (const unsigned char *)(uintptr_t)path_address;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint64_t index = 0; index < path_length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t hmje_host_path_slot_hash(uint64_t path_address, uint64_t path_length) {
    return hmje_posix_path_slot_hash(path_address, path_length);
}

static char *hmje_posix_copy_path(uint64_t address, uint64_t length) {
    if (address == 0 || length == 0 || length >= 65536) return NULL;
    const void *source = (const void *)(uintptr_t)address;
    if (memchr(source, '\0', (size_t)length) != NULL) return NULL;
    char *result = (char *)malloc((size_t)length + 1);
    if (result == NULL) return NULL;
    memcpy(result, source, (size_t)length);
    result[length] = '\0';
    return result;
}

/* Stream bytes in native code: clang/lld can exceed 100 MB.  A path or size
 * fingerprint cannot detect same-size in-place tool upgrades. */
uint64_t hmje_posix_file_content_hash(uint64_t address, uint64_t length) {
    char *path = hmje_posix_copy_path(address, length);
    if (path == NULL) return 0;
    FILE *stream = fopen(path, "rb");
    free(path);
    if (stream == NULL) return 0;
    unsigned char buffer[65536];
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
        for (size_t index = 0; index < count; ++index) {
            hash ^= buffer[index];
            hash *= UINT64_C(1099511628211);
        }
    }
    int failed = ferror(stream);
    if (fclose(stream) != 0) failed = 1;
    return failed ? 0 : (hash == 0 ? 1 : hash);
}

uint64_t hmje_host_file_content_hash(uint64_t address, uint64_t length) {
    return hmje_posix_file_content_hash(address, length);
}

/* Query the driver without a shell.  --version fingerprints dispatching stubs
 * such as /usr/bin/clang; -print-prog-name additionally hashes the selected real
 * compiler/linker bytes.  A failed query is never a reusable fingerprint. */
uint64_t hmje_posix_tool_query_hash(uint64_t address, uint64_t length,
                                   uint64_t option_address, uint64_t option_length) {
    char *tool = hmje_posix_copy_path(address, length);
    char *option = hmje_posix_copy_path(option_address, option_length);
    if (tool == NULL || option == NULL) { free(tool); free(option); return 0; }
    int channels[2];
    if (pipe(channels) != 0) { free(tool); free(option); return 0; }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) { close(channels[0]); close(channels[1]); free(tool); free(option); return 0; }
    error = posix_spawn_file_actions_adddup2(&actions, channels[1], STDOUT_FILENO);
    if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, channels[1], STDERR_FILENO);
    if (error == 0) error = posix_spawn_file_actions_addclose(&actions, channels[0]);
    if (error == 0) error = posix_spawn_file_actions_addclose(&actions, channels[1]);
    char *argv[] = {tool, option, NULL};
#if defined(__APPLE__)
    char **environment = *_NSGetEnviron();
#else
    extern char **environ;
    char **environment = environ;
#endif
    pid_t child = 0;
    if (error == 0) error = posix_spawnp(&child, tool, &actions, NULL, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    free(tool); free(option); close(channels[1]);
    if (error != 0) { close(channels[0]); return 0; }
    uint64_t hash = UINT64_C(1469598103934665603);
    char path[65536]; size_t saved = 0;
    unsigned char buffer[4096];
    for (;;) {
        ssize_t count = read(channels[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (count == 0) break;
        for (ssize_t index = 0; index < count; ++index) {
            hash ^= buffer[index]; hash *= UINT64_C(1099511628211);
            if (saved < sizeof(path) - 1) path[saved++] = (char)buffer[index];
        }
    }
    close(channels[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = errno; break;
    }
    if (error != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;
    while (saved > 0 && (path[saved - 1] == '\n' || path[saved - 1] == '\r')) --saved;
    path[saved] = '\0';
    if (saved > 0 && saved < sizeof(path) - 1 && path[0] == '/') {
        uint64_t contents = hmje_posix_file_content_hash((uint64_t)(uintptr_t)path, saved);
        if (contents == 0) return 0;
        hash ^= contents; hash *= UINT64_C(1099511628211);
    }
    return hash == 0 ? 1 : hash;
}

uint64_t hmje_host_tool_query_hash(uint64_t address, uint64_t length,
                                   uint64_t option_address, uint64_t option_length) {
    return hmje_posix_tool_query_hash(address, length, option_address, option_length);
}

int64_t hmje_posix_file_copy_atomic(
    uint64_t source_address,
    uint64_t source_length,
    uint64_t target_address,
    uint64_t target_length,
    uint64_t executable
) {
    char *source = hmje_posix_copy_path(source_address, source_length);
    char *target = hmje_posix_copy_path(target_address, target_length);
    if (source == NULL || target == NULL) {
        free(source);
        free(target);
        return -EINVAL;
    }
    size_t temporary_length = strlen(target) + 64;
    char *temporary = (char *)malloc(temporary_length);
    if (temporary == NULL) {
        free(source);
        free(target);
        return -ENOMEM;
    }
    snprintf(temporary, temporary_length, "%s.hmje-copy-%ld", target, (long)getpid());

    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        int error = errno;
        free(source); free(target); free(temporary);
        return -(int64_t)error;
    }
    struct stat metadata;
    if (fstat(input, &metadata) != 0) {
        int error = errno;
        close(input); free(source); free(target); free(temporary);
        return -(int64_t)error;
    }
    /* Products must stay owner-writable even when the source is read-only (platform
       libraries are installed read-only): xattr -c, codesign and later rebuilds rewrite them. */
    mode_t mode = (metadata.st_mode & 0777) | S_IWUSR;
    if (executable != 0) mode |= 0111;
    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (output < 0) {
        int error = errno;
        close(input); free(source); free(target); free(temporary);
        return -(int64_t)error;
    }
    int error = 0;
    unsigned char buffer[65536];
    for (;;) {
        ssize_t read_count = read(input, buffer, sizeof(buffer));
        if (read_count == 0) break;
        if (read_count < 0) {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        ssize_t offset = 0;
        while (offset < read_count) {
            ssize_t written = write(output, buffer + offset, (size_t)(read_count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                error = written < 0 ? errno : EIO;
                break;
            }
            offset += written;
        }
        if (error != 0) break;
    }
    if (error == 0 && fchmod(output, mode) != 0) error = errno;
    if (error == 0 && fsync(output) != 0) error = errno;
    if (close(input) != 0 && error == 0) error = errno;
    if (close(output) != 0 && error == 0) error = errno;
    if (error == 0 && rename(temporary, target) != 0) error = errno;
    if (error != 0) unlink(temporary);
    free(source); free(target); free(temporary);
    return error == 0 ? 0 : -(int64_t)error;
}

int64_t hmje_host_file_copy_atomic(uint64_t source_address, uint64_t source_length,
                                   uint64_t target_address, uint64_t target_length,
                                   uint64_t executable) {
    return hmje_posix_file_copy_atomic(source_address, source_length,
                                       target_address, target_length, executable);
}

int64_t hmje_host_file_replace_atomic(uint64_t source_address, uint64_t source_length,
                                      uint64_t target_address, uint64_t target_length) {
    char *source = hmje_posix_copy_path(source_address, source_length);
    char *target = hmje_posix_copy_path(target_address, target_length);
    if (!source || !target) { free(source); free(target); return -EINVAL; }
    int64_t result = rename(source, target) == 0 ? 0 : -(int64_t)errno;
    free(source); free(target);
    return result;
}

/* A nonempty .app cannot be replaced with ordinary rename. Darwin's atomic
 * exchange keeps the old or new complete bundle visible, without a gap.
 * The caller stages on the output filesystem and removes the exchanged old
 * tree after success. This promises atomic visibility, not crash persistence. */
int64_t hmje_host_bundle_publish_atomic(uint64_t source_address, uint64_t source_length,
                                        uint64_t target_address, uint64_t target_length) {
    char *source = hmje_posix_copy_path(source_address, source_length);
    char *target = hmje_posix_copy_path(target_address, target_length);
    if (!source || !target) { free(source); free(target); return -EINVAL; }
    int result;
#if defined(__APPLE__)
    result = renamex_np(source, target, RENAME_SWAP);
    if (result != 0 && errno == ENOENT) result = rename(source, target);
#else
    result = rename(source, target);
#endif
    int64_t status = result == 0 ? 0 : -(int64_t)errno;
    free(source); free(target);
    return status;
}
