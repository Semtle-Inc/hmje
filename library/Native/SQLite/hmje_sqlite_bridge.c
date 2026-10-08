#if defined(__linux__) && !defined(_GNU_SOURCE)
/* lstat/mkstemp/clock_gettime와 renameat2 선언보다 먼저 적용하며 이 C 파일에만 한정한다. */
#define _GNU_SOURCE 1
#endif

#include "hmje_sqlite_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <sys/acl.h>
#endif

#include <sqlite3.h>

#ifndef HMJE_SQLITE_TEST_COMMIT_FAULT_MODE
#define HMJE_SQLITE_TEST_COMMIT_FAULT_MODE 0
#endif

#ifndef HMJE_SQLITE_TEST_COMMIT_FAULT_AT
#define HMJE_SQLITE_TEST_COMMIT_FAULT_AT 0
#endif

#ifndef HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS
#define HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS 0
#endif

#ifndef HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP
#define HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP 0
#endif

#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE < 0 || \
    HMJE_SQLITE_TEST_COMMIT_FAULT_MODE > 2
#error "HMJE_SQLITE_TEST_COMMIT_FAULT_MODE must be 0, 1, or 2"
#endif

#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE != 0 && \
    HMJE_SQLITE_TEST_COMMIT_FAULT_AT <= 0
#error "HMJE_SQLITE_TEST_COMMIT_FAULT_AT must be positive for a fault build"
#endif

#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE == 1
__attribute__((used, visibility("default")))
const char hmje_sqlite_commit_fault_test_marker[] =
    "hmje-test-only-artifact:sqlite-commit-before";
#elif HMJE_SQLITE_TEST_COMMIT_FAULT_MODE == 2
__attribute__((used, visibility("default")))
const char hmje_sqlite_commit_fault_test_marker[] =
    "hmje-test-only-artifact:sqlite-commit-after";
#endif

#if HMJE_SQLITE_TEST_COMMIT_FAULT_MODE != 0
static uint64_t hmje_sqlite_test_commit_count = 0;
#endif

#if HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS < 0 || \
    HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS > 60000
#error "HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS must be 0..60000"
#endif

#if HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP < 0 || \
    HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP > 1
#error "HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP must be 0 or 1"
#endif

#if HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS != 0 || \
    HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP != 0
__attribute__((used, visibility("default")))
const char hmje_sqlite_restore_capture_delay_test_marker[] =
    "hmje-test-only-artifact:sqlite-restore-capture-delay";
#endif

#if !defined(O_NOFOLLOW)
#error "hmje-sqlite restore requires O_NOFOLLOW"
#endif

#if !defined(O_DIRECTORY)
#error "hmje-sqlite restore requires O_DIRECTORY"
#endif

#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif
#include "hmje_sqlite_core.inc"
#include "hmje_sqlite_operations.inc"

enum {
    HMJE_SHA256_BLOCK_SIZE = 64,
    HMJE_SHA256_DIGEST_SIZE = 32
};

typedef struct hmje_sha256_context {
    uint32_t state[8];
    uint64_t total_bytes;
    unsigned char block[HMJE_SHA256_BLOCK_SIZE];
    size_t block_length;
} hmje_sha256_context;

typedef struct restore_source_snapshot {
    char *path;
    int descriptor;
    struct stat status;
    unsigned char digest[HMJE_SHA256_DIGEST_SIZE];
} restore_source_snapshot;

static uint32_t rotate_right_u32(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t load_be_u32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void store_be_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24U);
    bytes[1] = (unsigned char)(value >> 16U);
    bytes[2] = (unsigned char)(value >> 8U);
    bytes[3] = (unsigned char)value;
}

static void sha256_transform(hmje_sha256_context *context,
                             const unsigned char *block) {
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491),
        UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
        UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
        UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
        UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
        UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
        UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
        UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624),
        UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
        UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
        UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
        UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
    };
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0; index < 16; index += 1) {
        schedule[index] = load_be_u32(block + (index * 4));
    }
    for (index = 16; index < 64; index += 1) {
        uint32_t left = schedule[index - 15];
        uint32_t right = schedule[index - 2];
        uint32_t sigma_zero =
            rotate_right_u32(left, 7U) ^
            rotate_right_u32(left, 18U) ^ (left >> 3U);
        uint32_t sigma_one =
            rotate_right_u32(right, 17U) ^
            rotate_right_u32(right, 19U) ^ (right >> 10U);
        schedule[index] = schedule[index - 16] + sigma_zero +
                          schedule[index - 7] + sigma_one;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0; index < 64; index += 1) {
        uint32_t sigma_one =
            rotate_right_u32(e, 6U) ^
            rotate_right_u32(e, 11U) ^
            rotate_right_u32(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t first = h + sigma_one + choose +
                         constants[index] + schedule[index];
        uint32_t sigma_zero =
            rotate_right_u32(a, 2U) ^
            rotate_right_u32(a, 13U) ^
            rotate_right_u32(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = sigma_zero + majority;

        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_initialize(hmje_sha256_context *context) {
    static const uint32_t initial_state[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->total_bytes = 0;
    context->block_length = 0;
}

static int sha256_update(hmje_sha256_context *context,
                         const unsigned char *bytes, size_t length) {
    size_t copied;
    if (context == NULL || (length != 0 && bytes == NULL) ||
        (uint64_t)length > UINT64_MAX - context->total_bytes) {
        return SQLITE_TOOBIG;
    }
    context->total_bytes += (uint64_t)length;
    while (length != 0) {
        size_t available =
            (size_t)HMJE_SHA256_BLOCK_SIZE - context->block_length;
        copied = length < available ? length : available;
        memcpy(context->block + context->block_length, bytes, copied);
        context->block_length += copied;
        bytes += copied;
        length -= copied;
        if (context->block_length == (size_t)HMJE_SHA256_BLOCK_SIZE) {
            sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
    return SQLITE_OK;
}

static int sha256_finish(hmje_sha256_context *context,
                         unsigned char digest[HMJE_SHA256_DIGEST_SIZE]) {
    uint64_t bit_length;
    size_t index;
    if (context == NULL || digest == NULL ||
        context->total_bytes > UINT64_MAX / UINT64_C(8)) {
        return SQLITE_TOOBIG;
    }
    bit_length = context->total_bytes * UINT64_C(8);
    context->block[context->block_length] = 0x80U;
    context->block_length += 1;
    if (context->block_length > 56) {
        memset(context->block + context->block_length, 0,
               (size_t)HMJE_SHA256_BLOCK_SIZE - context->block_length);
        sha256_transform(context, context->block);
        context->block_length = 0;
    }
    memset(context->block + context->block_length, 0,
           56 - context->block_length);
    for (index = 0; index < 8; index += 1) {
        context->block[56 + index] =
            (unsigned char)(bit_length >> ((7U - (unsigned int)index) * 8U));
    }
    sha256_transform(context, context->block);
    for (index = 0; index < 8; index += 1) {
        store_be_u32(digest + (index * 4), context->state[index]);
    }
    return SQLITE_OK;
}

static int sha256_self_test(void) {
    static const unsigned char expected[HMJE_SHA256_DIGEST_SIZE] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
    };
    static const unsigned char input[] = {'a', 'b', 'c'};
    hmje_sha256_context context;
    unsigned char digest[HMJE_SHA256_DIGEST_SIZE];
    int result;
    sha256_initialize(&context);
    result = sha256_update(&context, input, sizeof(input));
    if (result != SQLITE_OK) {
        return result;
    }
    result = sha256_finish(&context, digest);
    if (result != SQLITE_OK) {
        return result;
    }
    return memcmp(digest, expected, sizeof(expected)) == 0
               ? SQLITE_OK
               : SQLITE_IOERR;
}

/*
 * HMJE 문자열에는 NUL 종단자가 없다. C 문자열 API에 넘길 값은 항상 새 버퍼에
 * 복사하고, 중간 NUL은 묵시적인 잘림 대신 SQLITE_MISUSE로 거부한다.
 */
static int copy_operational_path(uint64_t address, uint64_t length,
                                 char **output) {
    int result = copy_c_string(address, length, output);
    if (result != SQLITE_OK) {
        return result;
    }
    /*
     * 운영 파일 API는 실제 파일 경로만 받는다. SQLite URI와 :memory:를
     * 허용하면 동일 파일 검사와 restore의 목적 파일 계약을 우회할 수 있다.
     */
    if (length == 0 || strcmp(*output, ":memory:") == 0 ||
        strncmp(*output, "file:", 5) == 0) {
        free(*output);
        *output = NULL;
        return SQLITE_MISUSE;
    }
    return SQLITE_OK;
}

static int paths_reference_same_file(const char *left, const char *right) {
    struct stat left_status;
    struct stat right_status;

    if (left == NULL || right == NULL || left[0] == '\0' ||
        right[0] == '\0') {
        return 0;
    }
    if (strcmp(left, right) == 0) {
        return 1;
    }
    if (stat(left, &left_status) != 0 || stat(right, &right_status) != 0) {
        return 0;
    }
    return left_status.st_dev == right_status.st_dev &&
           left_status.st_ino == right_status.st_ino;
}

static int databases_reference_same_file(sqlite3 *left, sqlite3 *right) {
    return paths_reference_same_file(sqlite3_db_filename(left, "main"),
                                     sqlite3_db_filename(right, "main"));
}

static int path_entry_exists(const char *path, int *exists) {
    struct stat status;
    if (exists == NULL) {
        return SQLITE_MISUSE;
    }
    *exists = 0;
    if (lstat(path, &status) == 0) {
        *exists = 1;
        return SQLITE_OK;
    }
    return errno == ENOENT ? SQLITE_OK : SQLITE_CANTOPEN;
}

static int unlink_path_with_suffix(const char *path, const char *suffix) {
    size_t path_length;
    size_t suffix_length;
    char *full_path;
    int result = SQLITE_OK;

    if (path == NULL || suffix == NULL) {
        return SQLITE_MISUSE;
    }
    path_length = strlen(path);
    suffix_length = strlen(suffix);
    if (path_length > SIZE_MAX - suffix_length - 1) {
        return SQLITE_TOOBIG;
    }
    full_path = (char *)malloc(path_length + suffix_length + 1);
    if (full_path == NULL) {
        return SQLITE_NOMEM;
    }
    memcpy(full_path, path, path_length);
    memcpy(full_path + path_length, suffix, suffix_length + 1);
    if (unlink(full_path) != 0 && errno != ENOENT) {
        result = SQLITE_IOERR;
    }
    free(full_path);
    return result;
}

static int remove_temporary_database_files(const char *path,
                                           int remove_main_file) {
    static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
    size_t index;
    int result = SQLITE_OK;

    for (index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]);
         index += 1) {
        int unlink_result = unlink_path_with_suffix(path, suffixes[index]);
        if (result == SQLITE_OK && unlink_result != SQLITE_OK) {
            result = unlink_result;
        }
    }
    if (remove_main_file != 0 &&
        unlink(path) != 0 && errno != ENOENT && result == SQLITE_OK) {
        result = SQLITE_IOERR;
    }
    return result;
}

static int clear_descriptor_extended_acl(int descriptor) {
#if defined(__APPLE__)
    acl_t empty_acl = acl_init(0);
    int result;
    int free_result;
    if (empty_acl == NULL) {
        return SQLITE_IOERR;
    }
    result = acl_set_fd_np(descriptor, empty_acl, ACL_TYPE_EXTENDED) == 0
                 ? SQLITE_OK
                 : SQLITE_IOERR;
    free_result = acl_free(empty_acl);
    if (result == SQLITE_OK && free_result != 0) {
        result = SQLITE_IOERR;
    }
    return result;
#else
    (void)descriptor;
    return SQLITE_OK;
#endif
}

static int require_descriptor_without_extended_acl(int descriptor) {
#if defined(__APPLE__)
    acl_t acl;

    errno = 0;
    acl = acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    if (acl == NULL) {
        return errno == ENOENT ? SQLITE_OK : SQLITE_CANTOPEN;
    }
    (void)acl_free(acl);
    return SQLITE_CANTOPEN;
#else
    (void)descriptor;
    return SQLITE_OK;
#endif
}

static int create_sibling_temporary_path(const char *destination_path,
                                         char **temporary_path) {
    static const char suffix[] = ".hmje-partial-XXXXXX";
    size_t destination_length;
    size_t total_length;
    int descriptor;

    if (destination_path == NULL || temporary_path == NULL) {
        return SQLITE_MISUSE;
    }
    *temporary_path = NULL;
    destination_length = strlen(destination_path);
    if (destination_length > SIZE_MAX - sizeof(suffix)) {
        return SQLITE_TOOBIG;
    }
    total_length = destination_length + sizeof(suffix);
    *temporary_path = (char *)malloc(total_length);
    if (*temporary_path == NULL) {
        return SQLITE_NOMEM;
    }
    memcpy(*temporary_path, destination_path, destination_length);
    memcpy(*temporary_path + destination_length, suffix, sizeof(suffix));
    descriptor = mkstemp(*temporary_path);
    if (descriptor < 0) {
        int saved_errno = errno;
        free(*temporary_path);
        *temporary_path = NULL;
        if (saved_errno == EACCES || saved_errno == ENOENT ||
            saved_errno == ENOTDIR || saved_errno == ELOOP) {
            return SQLITE_CANTOPEN;
        }
        return SQLITE_IOERR;
    }
    if (fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        (void)close(descriptor);
        (void)unlink(*temporary_path);
        free(*temporary_path);
        *temporary_path = NULL;
        return SQLITE_IOERR;
    }
    if (clear_descriptor_extended_acl(descriptor) != SQLITE_OK) {
        (void)close(descriptor);
        (void)unlink(*temporary_path);
        free(*temporary_path);
        *temporary_path = NULL;
        return SQLITE_IOERR;
    }
    if (close(descriptor) != 0) {
        (void)unlink(*temporary_path);
        free(*temporary_path);
        *temporary_path = NULL;
        return SQLITE_IOERR;
    }
    return SQLITE_OK;
}

static int sync_file_to_stable_storage(const char *path) {
    int descriptor;
    int result = SQLITE_OK;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return SQLITE_CANTOPEN;
    }
#if defined(F_FULLFSYNC)
    if (fcntl(descriptor, F_FULLFSYNC) != 0) {
        if (errno != EINVAL && errno != ENOTSUP && errno != ENOTTY) {
            result = SQLITE_IOERR;
        } else if (fsync(descriptor) != 0) {
            result = SQLITE_IOERR;
        }
    }
#else
    if (fsync(descriptor) != 0) {
        result = SQLITE_IOERR;
    }
#endif
    if (close(descriptor) != 0 && result == SQLITE_OK) {
        result = SQLITE_IOERR;
    }
    return result;
}

static int copy_parent_directory_path(const char *path, char **parent_path) {
    const char *slash;
    size_t length;
    if (path == NULL || parent_path == NULL) {
        return SQLITE_MISUSE;
    }
    *parent_path = NULL;
    slash = strrchr(path, '/');
    if (slash == NULL) {
        path = ".";
        length = 1;
    } else if (slash == path) {
        length = 1;
    } else {
        length = (size_t)(slash - path);
    }
    *parent_path = (char *)malloc(length + 1);
    if (*parent_path == NULL) {
        return SQLITE_NOMEM;
    }
    memcpy(*parent_path, path, length);
    (*parent_path)[length] = '\0';
    return SQLITE_OK;
}

static const char *path_basename(const char *path) {
    const char *slash;
    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int create_sibling_source_snapshot(const char *destination_path,
                                          char **snapshot_path,
                                          int *snapshot_descriptor) {
    static const char suffix[] = ".hmje-source-XXXXXX";
    size_t destination_length;
    size_t total_length;
    int descriptor;

    if (destination_path == NULL || snapshot_path == NULL ||
        snapshot_descriptor == NULL) {
        return SQLITE_MISUSE;
    }
    *snapshot_path = NULL;
    *snapshot_descriptor = -1;
    destination_length = strlen(destination_path);
    if (destination_length > SIZE_MAX - sizeof(suffix)) {
        return SQLITE_TOOBIG;
    }
    total_length = destination_length + sizeof(suffix);
    *snapshot_path = (char *)malloc(total_length);
    if (*snapshot_path == NULL) {
        return SQLITE_NOMEM;
    }
    memcpy(*snapshot_path, destination_path, destination_length);
    memcpy(*snapshot_path + destination_length, suffix, sizeof(suffix));
    descriptor = mkstemp(*snapshot_path);
    if (descriptor < 0) {
        int saved_errno = errno;
        free(*snapshot_path);
        *snapshot_path = NULL;
        if (saved_errno == EACCES || saved_errno == ENOENT ||
            saved_errno == ENOTDIR || saved_errno == ELOOP) {
            return SQLITE_CANTOPEN;
        }
        return SQLITE_IOERR;
    }
    if (fchmod(descriptor, S_IRUSR | S_IWUSR) != 0 ||
        clear_descriptor_extended_acl(descriptor) != SQLITE_OK) {
        (void)close(descriptor);
        (void)unlink(*snapshot_path);
        free(*snapshot_path);
        *snapshot_path = NULL;
        return SQLITE_IOERR;
    }
    *snapshot_descriptor = descriptor;
    return SQLITE_OK;
}

static int statuses_have_same_identity_and_size(const struct stat *left,
                                                const struct stat *right) {
    return left != NULL && right != NULL &&
           left->st_dev == right->st_dev &&
           left->st_ino == right->st_ino &&
           left->st_size == right->st_size;
}

static int require_private_regular_descriptor(
    int descriptor, nlink_t expected_link_count, struct stat *status) {
    struct stat current;
    int acl_result;
    if (fstat(descriptor, &current) != 0) {
        return SQLITE_CANTOPEN;
    }
    if (!S_ISREG(current.st_mode) ||
        current.st_uid != geteuid() ||
        current.st_nlink != expected_link_count ||
        current.st_size < 0 ||
        (current.st_mode & (S_IRWXG | S_IRWXO | S_IXUSR)) != 0 ||
        (current.st_mode & S_IRUSR) == 0) {
        return SQLITE_CANTOPEN;
    }
    acl_result = require_descriptor_without_extended_acl(descriptor);
    if (acl_result != SQLITE_OK) {
        return acl_result;
    }
    if (status != NULL) {
        *status = current;
    }
    return SQLITE_OK;
}

static int require_trusted_source_parent(int descriptor) {
    struct stat status;
    uid_t effective_user = geteuid();
    if (fstat(descriptor, &status) != 0 ||
        !S_ISDIR(status.st_mode) ||
        (status.st_uid != effective_user && status.st_uid != (uid_t)0) ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return SQLITE_CANTOPEN;
    }
    return SQLITE_OK;
}

static int require_source_entry_identity(int parent_descriptor,
                                         const char *basename,
                                         const struct stat *expected) {
    struct stat entry_status;
    if (basename == NULL || basename[0] == '\0' || expected == NULL) {
        return SQLITE_CANTOPEN;
    }
    if (fstatat(parent_descriptor, basename, &entry_status,
                AT_SYMLINK_NOFOLLOW) != 0) {
        return SQLITE_CANTOPEN;
    }
    if (!S_ISREG(entry_status.st_mode) ||
        !statuses_have_same_identity_and_size(&entry_status, expected)) {
        return SQLITE_BUSY;
    }
    return SQLITE_OK;
}

static int require_source_sidecars_absent(int parent_descriptor,
                                          const char *basename) {
    static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
    size_t basename_length;
    size_t index;
    if (basename == NULL || basename[0] == '\0') {
        return SQLITE_CANTOPEN;
    }
    basename_length = strlen(basename);
    for (index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]);
         index += 1) {
        const char *suffix = suffixes[index];
        size_t suffix_length = strlen(suffix);
        char *sidecar_name;
        struct stat sidecar_status;
        int saved_errno;

        if (basename_length > SIZE_MAX - suffix_length - 1) {
            return SQLITE_TOOBIG;
        }
        sidecar_name =
            (char *)malloc(basename_length + suffix_length + 1);
        if (sidecar_name == NULL) {
            return SQLITE_NOMEM;
        }
        memcpy(sidecar_name, basename, basename_length);
        memcpy(sidecar_name + basename_length, suffix, suffix_length + 1);
        errno = 0;
        if (fstatat(parent_descriptor, sidecar_name, &sidecar_status,
                    AT_SYMLINK_NOFOLLOW) == 0) {
            free(sidecar_name);
            return SQLITE_BUSY;
        }
        saved_errno = errno;
        free(sidecar_name);
        if (saved_errno != ENOENT) {
            return SQLITE_CANTOPEN;
        }
    }
    return SQLITE_OK;
}

static int write_all_to_descriptor(int descriptor,
                                   const unsigned char *bytes,
                                   size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t amount = write(descriptor, bytes + written,
                               length - written);
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SQLITE_IOERR;
        }
        if (amount == 0) {
            return SQLITE_IOERR;
        }
        written += (size_t)amount;
    }
    return SQLITE_OK;
}

static int digest_descriptor(
    int descriptor,
    unsigned char digest[HMJE_SHA256_DIGEST_SIZE]) {
    unsigned char buffer[65536];
    hmje_sha256_context context;
    int result;
    if (lseek(descriptor, (off_t)0, SEEK_SET) < 0) {
        return SQLITE_IOERR;
    }
    sha256_initialize(&context);
    for (;;) {
        ssize_t amount = read(descriptor, buffer, sizeof(buffer));
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SQLITE_IOERR;
        }
        if (amount == 0) {
            break;
        }
        result = sha256_update(&context, buffer, (size_t)amount);
        if (result != SQLITE_OK) {
            return result;
        }
    }
    return sha256_finish(&context, digest);
}

static int copy_descriptor_with_digest(
    int source_descriptor, int destination_descriptor,
    unsigned char digest[HMJE_SHA256_DIGEST_SIZE]) {
    unsigned char buffer[65536];
    hmje_sha256_context context;
    int result;
    if (lseek(source_descriptor, (off_t)0, SEEK_SET) < 0 ||
        lseek(destination_descriptor, (off_t)0, SEEK_SET) < 0 ||
        ftruncate(destination_descriptor, (off_t)0) != 0) {
        return SQLITE_IOERR;
    }
    sha256_initialize(&context);
    for (;;) {
        ssize_t amount = read(source_descriptor, buffer, sizeof(buffer));
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SQLITE_IOERR;
        }
        if (amount == 0) {
            break;
        }
        result = sha256_update(&context, buffer, (size_t)amount);
        if (result != SQLITE_OK) {
            return result;
        }
        result = write_all_to_descriptor(
            destination_descriptor, buffer, (size_t)amount);
        if (result != SQLITE_OK) {
            return result;
        }
    }
    return sha256_finish(&context, digest);
}

static int restore_capture_test_delay(void) {
#if HMJE_SQLITE_TEST_RESTORE_CAPTURE_STOP != 0
    if (raise(SIGSTOP) != 0) {
        return SQLITE_IOERR;
    }
#endif
#if HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS != 0
    struct timespec remaining = {
        (time_t)(HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS / 1000),
        (long)((HMJE_SQLITE_TEST_RESTORE_CAPTURE_DELAY_MS % 1000) *
               1000000L)
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            return SQLITE_IOERR;
        }
    }
#endif
    return SQLITE_OK;
}

static int verify_restore_source_snapshot(
    const restore_source_snapshot *snapshot, nlink_t expected_link_count) {
    struct stat current;
    unsigned char digest[HMJE_SHA256_DIGEST_SIZE];
    int result;
    if (snapshot == NULL || snapshot->descriptor < 0) {
        return SQLITE_MISUSE;
    }
    result = require_private_regular_descriptor(
        snapshot->descriptor, expected_link_count, &current);
    if (result != SQLITE_OK) {
        return result;
    }
    if (!statuses_have_same_identity_and_size(&current,
                                               &snapshot->status)) {
        return SQLITE_IOERR;
    }
    result = digest_descriptor(snapshot->descriptor, digest);
    if (result != SQLITE_OK) {
        return result;
    }
    return memcmp(digest, snapshot->digest, sizeof(digest)) == 0
               ? SQLITE_OK
               : SQLITE_IOERR;
}

static int capture_restore_source(
    const char *source_path, const char *destination_path,
    restore_source_snapshot *snapshot) {
    char *parent_path = NULL;
    const char *basename;
    int parent_descriptor = -1;
    int source_descriptor = -1;
    struct stat source_before;
    struct stat source_middle;
    struct stat source_after;
    struct stat snapshot_status;
    unsigned char first_digest[HMJE_SHA256_DIGEST_SIZE];
    unsigned char second_digest[HMJE_SHA256_DIGEST_SIZE];
    unsigned char snapshot_digest[HMJE_SHA256_DIGEST_SIZE];
    int result;
    int close_result;

    if (source_path == NULL || destination_path == NULL ||
        snapshot == NULL) {
        return SQLITE_MISUSE;
    }
    snapshot->path = NULL;
    snapshot->descriptor = -1;
    memset(&snapshot->status, 0, sizeof(snapshot->status));
    memset(snapshot->digest, 0, sizeof(snapshot->digest));
    result = sha256_self_test();
    if (result != SQLITE_OK) {
        return result;
    }
    basename = path_basename(source_path);
    result = copy_parent_directory_path(source_path, &parent_path);
    if (result != SQLITE_OK) {
        return result;
    }
    parent_descriptor =
        open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    free(parent_path);
    parent_path = NULL;
    if (parent_descriptor < 0) {
        return SQLITE_CANTOPEN;
    }
    result = require_trusted_source_parent(parent_descriptor);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = require_source_sidecars_absent(parent_descriptor, basename);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    source_descriptor =
        openat(parent_descriptor, basename,
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source_descriptor < 0) {
        result = SQLITE_CANTOPEN;
        goto capture_close_source_parent;
    }
    result = require_private_regular_descriptor(
        source_descriptor, (nlink_t)1, &source_before);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = require_source_entry_identity(
        parent_descriptor, basename, &source_before);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = create_sibling_source_snapshot(
        destination_path, &snapshot->path, &snapshot->descriptor);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = copy_descriptor_with_digest(
        source_descriptor, snapshot->descriptor, first_digest);
    if (result != SQLITE_OK || fsync(snapshot->descriptor) != 0) {
        if (result == SQLITE_OK) {
            result = SQLITE_IOERR;
        }
        goto capture_close_source_parent;
    }
    result = require_private_regular_descriptor(
        source_descriptor, (nlink_t)1, &source_middle);
    if (result != SQLITE_OK ||
        !statuses_have_same_identity_and_size(
            &source_before, &source_middle)) {
        if (result == SQLITE_OK) {
            result = SQLITE_BUSY;
        }
        goto capture_close_source_parent;
    }
    result = require_source_entry_identity(
        parent_descriptor, basename, &source_middle);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = require_source_sidecars_absent(parent_descriptor, basename);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = restore_capture_test_delay();
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = digest_descriptor(source_descriptor, second_digest);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = require_private_regular_descriptor(
        source_descriptor, (nlink_t)1, &source_after);
    if (result != SQLITE_OK ||
        !statuses_have_same_identity_and_size(
            &source_before, &source_after)) {
        if (result == SQLITE_OK) {
            result = SQLITE_BUSY;
        }
        goto capture_close_source_parent;
    }
    result = require_source_entry_identity(
        parent_descriptor, basename, &source_after);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    result = require_source_sidecars_absent(parent_descriptor, basename);
    if (result != SQLITE_OK) {
        goto capture_close_source_parent;
    }
    if (memcmp(first_digest, second_digest, sizeof(first_digest)) != 0) {
        result = SQLITE_BUSY;
        goto capture_close_source_parent;
    }
    if (fchmod(snapshot->descriptor, S_IRUSR | S_IWUSR) != 0 ||
        fsync(snapshot->descriptor) != 0) {
        result = SQLITE_IOERR;
        goto capture_close_source_parent;
    }
    result = require_private_regular_descriptor(
        snapshot->descriptor, (nlink_t)1, &snapshot_status);
    if (result != SQLITE_OK ||
        snapshot_status.st_size != source_before.st_size) {
        if (result == SQLITE_OK) {
            result = SQLITE_IOERR;
        }
        goto capture_close_source_parent;
    }
    result = digest_descriptor(snapshot->descriptor, snapshot_digest);
    if (result != SQLITE_OK ||
        memcmp(first_digest, snapshot_digest, sizeof(first_digest)) != 0) {
        if (result == SQLITE_OK) {
            result = SQLITE_IOERR;
        }
        goto capture_close_source_parent;
    }
    snapshot->status = snapshot_status;
    memcpy(snapshot->digest, snapshot_digest, sizeof(snapshot->digest));

capture_close_source_parent:
    if (source_descriptor >= 0) {
        close_result = close(source_descriptor);
        source_descriptor = -1;
        if (result == SQLITE_OK && close_result != 0) {
            result = SQLITE_IOERR;
        }
    }
    close_result = close(parent_descriptor);
    parent_descriptor = -1;
    if (result == SQLITE_OK && close_result != 0) {
        result = SQLITE_IOERR;
    }
    if (result != SQLITE_OK && snapshot->descriptor >= 0) {
        (void)close(snapshot->descriptor);
        snapshot->descriptor = -1;
    }
    if (result != SQLITE_OK && snapshot->path != NULL) {
        if (unlink(snapshot->path) != 0 && errno != ENOENT) {
            result = SQLITE_IOERR;
        }
        free(snapshot->path);
        snapshot->path = NULL;
    }
    return result;
}

static int sync_parent_directory(const char *path) {
    char *parent_path = NULL;
    int descriptor;
    int result = copy_parent_directory_path(path, &parent_path);
    if (result != SQLITE_OK) {
        return result;
    }
    descriptor = open(parent_path, O_RDONLY);
    free(parent_path);
    if (descriptor < 0) {
        return SQLITE_CANTOPEN;
    }
    result = fsync(descriptor) == 0 ? SQLITE_OK : SQLITE_IOERR;
    if (close(descriptor) != 0 && result == SQLITE_OK) {
        result = SQLITE_IOERR;
    }
    return result;
}

#if defined(__linux__) && defined(RENAME_NOREPLACE)
static int rename_no_replace(const char *temporary_path,
                             const char *destination_path) {
#if defined(__ANDROID__)
    /* Bionic exposes renameat2 only from API 30. The kernel syscall is also
     * usable by API 26 clients; its absence still takes the fail-closed path. */
#if defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, temporary_path,
                        AT_FDCWD, destination_path, RENAME_NOREPLACE);
#else
    errno = ENOSYS;
    return -1;
#endif
#else
    return renameat2(AT_FDCWD, temporary_path, AT_FDCWD,
                     destination_path, RENAME_NOREPLACE);
#endif
}
#endif

static int publish_no_replace(const char *temporary_path,
                              const char *destination_path) {
#if defined(__APPLE__) && defined(RENAME_EXCL)
    if (renameatx_np(AT_FDCWD, temporary_path, AT_FDCWD,
                     destination_path, RENAME_EXCL) == 0) {
        return SQLITE_OK;
    }
    if (errno == EEXIST) {
        return SQLITE_CONSTRAINT;
    }
    if (errno == EACCES || errno == ENOENT || errno == ENOTDIR ||
        errno == ELOOP) {
        return SQLITE_CANTOPEN;
    }
    return SQLITE_IOERR;
#elif defined(__linux__) && defined(RENAME_NOREPLACE)
    if (rename_no_replace(temporary_path, destination_path) == 0) {
        return SQLITE_OK;
    }
    if (errno == EEXIST) {
        return SQLITE_CONSTRAINT;
    }
    if (errno == EACCES || errno == ENOENT || errno == ENOTDIR ||
        errno == ELOOP) {
        return SQLITE_CANTOPEN;
    }
    /*
     * 지원하지 않는 kernel/filesystem에서 link+unlink로 조용히 의미를 바꾸지 않는다.
     * 원자적 no-replace rename을 제공하지 못하면 게시 전 상태로 fail-close한다.
     */
    if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) {
        return SQLITE_MISUSE;
    }
    return SQLITE_IOERR;
#else
    (void)temporary_path;
    (void)destination_path;
    return SQLITE_MISUSE;
#endif
}

static int configure_durable_destination(sqlite3 *database) {
    int result;
    if (database == NULL) {
        return SQLITE_MISUSE;
    }
    result = sqlite3_busy_timeout(database, 0);
    if (result != SQLITE_OK) {
        return result;
    }
    return sqlite3_exec(database, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
}

/*
 * 게시되는 새 데이터베이스는 한 파일만 원자적으로 이름을 바꿀 수 있어야 한다.
 * 원본이 WAL 모드이면 backup API가 목적 파일 헤더에도 WAL 모드를 남길 수 있으므로,
 * 복사 트랜잭션 뒤 rollback-journal 모드로 정상 전환됐는지 결과 행까지 확인한다.
 */
static int normalize_single_file_database(sqlite3 *database) {
    sqlite3_stmt *statement = NULL;
    const unsigned char *mode;
    int result;
    int finalize_result;

    if (database == NULL) {
        return SQLITE_MISUSE;
    }
    result = sqlite3_prepare_v2(database, "PRAGMA journal_mode=DELETE", -1,
                                &statement, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        mode = sqlite3_column_text(statement, 0);
        if (mode == NULL) {
            result = sqlite3_column_type(statement, 0) == SQLITE_NULL
                         ? SQLITE_ERROR
                         : SQLITE_NOMEM;
        } else if (sqlite3_stricmp((const char *)mode, "delete") != 0) {
            result = SQLITE_BUSY;
        } else {
            result = sqlite3_step(statement);
            if (result == SQLITE_DONE) {
                result = SQLITE_OK;
            }
        }
    }
    finalize_result = sqlite3_finalize(statement);
    if (result == SQLITE_OK && finalize_result != SQLITE_OK) {
        result = finalize_result;
    }
    return result;
}

static int require_single_file_restore_source(sqlite3 *database) {
    sqlite3_stmt *statement = NULL;
    const unsigned char *mode;
    int result;
    int finalize_result;

    if (database == NULL) {
        return SQLITE_MISUSE;
    }
    result = sqlite3_prepare_v2(database, "PRAGMA journal_mode", -1,
                                &statement, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        mode = sqlite3_column_text(statement, 0);
        if (mode == NULL) {
            result = sqlite3_column_type(statement, 0) == SQLITE_NULL
                         ? SQLITE_ERROR
                         : SQLITE_NOMEM;
        } else if (sqlite3_stricmp((const char *)mode, "delete") != 0) {
            result = SQLITE_BUSY;
        } else {
            result = sqlite3_step(statement);
            if (result == SQLITE_DONE) {
                result = SQLITE_OK;
            }
        }
    }
    finalize_result = sqlite3_finalize(statement);
    if (result == SQLITE_OK && finalize_result != SQLITE_OK) {
        result = finalize_result;
    }
    return result;
}

/*
 * 기존 목적 복원은 WAL의 reader/writer 공존 규칙을 그대로 두면 활성 reader를
 * 선행 탐지할 수 없다. rollback-journal로 바꾼 뒤 EXCLUSIVE locking mode에서
 * 빈 exclusive transaction을 한 번 끝내 lock을 close까지 보유한다.
 */
static int acquire_restore_exclusive_lock(sqlite3 *database) {
    sqlite3_stmt *statement = NULL;
    const unsigned char *mode;
    int result;
    int finalize_result;

    if (database == NULL) {
        return SQLITE_MISUSE;
    }
    result = sqlite3_prepare_v2(database, "PRAGMA locking_mode=EXCLUSIVE", -1,
                                &statement, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        mode = sqlite3_column_text(statement, 0);
        if (mode == NULL) {
            result = sqlite3_column_type(statement, 0) == SQLITE_NULL
                         ? SQLITE_ERROR
                         : SQLITE_NOMEM;
        } else if (sqlite3_stricmp((const char *)mode, "exclusive") != 0) {
            result = SQLITE_BUSY;
        } else {
            result = sqlite3_step(statement);
            if (result == SQLITE_DONE) {
                result = SQLITE_OK;
            }
        }
    }
    finalize_result = sqlite3_finalize(statement);
    if (result == SQLITE_OK && finalize_result != SQLITE_OK) {
        result = finalize_result;
    }
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_exec(database, "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_exec(database, "COMMIT", NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        (void)sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

int32_t hmje_sqlite_online_backup(
    uint64_t source_database, uint64_t destination_path_address,
    uint64_t destination_path_length, uint64_t maximum_duration_ms,
    uint64_t out_stage_address,
    uint64_t out_page_count_address, uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address) {
    sqlite3 *source = database_from_handle(source_database);
    sqlite3 *destination = NULL;
    const char *source_path;
    char *destination_path = NULL;
    char *temporary_path = NULL;
    int32_t *stage;
    uint64_t *page_count;
    uint64_t *copied_pages;
    int32_t *destination_state;
    int32_t *durability_state;
    int32_t temporary_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
    uint64_t problems = 0;
    int destination_exists = 0;
    int published = 0;
    int result;
    int close_result;

    result = initialize_operation_outputs(
        out_stage_address, out_page_count_address, out_copied_pages_address,
        out_destination_state_address, out_durability_state_address,
        &stage, &page_count, &copied_pages, &destination_state,
        &durability_state);
    if (result != SQLITE_OK) {
        return result;
    }
    *stage = HMJE_SQLITE_OPERATION_STAGE_PATH;
    if (source == NULL) {
        return SQLITE_MISUSE;
    }
    result = copy_operational_path(destination_path_address,
                                   destination_path_length,
                                   &destination_path);
    if (result != SQLITE_OK) {
        return result;
    }
    source_path = sqlite3_db_filename(source, "main");
    if (paths_reference_same_file(source_path, destination_path)) {
        free(destination_path);
        return SQLITE_MISUSE;
    }
    result = path_entry_exists(destination_path, &destination_exists);
    if (result != SQLITE_OK) {
        free(destination_path);
        return result;
    }
    if (destination_exists != 0) {
        free(destination_path);
        return SQLITE_CONSTRAINT;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_DESTINATION_OPEN;
    result = create_sibling_temporary_path(destination_path,
                                           &temporary_path);
    if (result != SQLITE_OK) {
        free(destination_path);
        return result;
    }
    result = sqlite3_open_v2(temporary_path, &destination,
                             SQLITE_OPEN_READWRITE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (result != SQLITE_OK) {
        if (destination != NULL) {
            (void)sqlite3_close(destination);
        }
        if (remove_temporary_database_files(temporary_path, 1) !=
            SQLITE_OK) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
            *destination_state = HMJE_SQLITE_DESTINATION_CHECK_REQUIRED;
            result = SQLITE_IOERR;
        }
        free(temporary_path);
        free(destination_path);
        return result;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
    result = configure_durable_destination(destination);
    if (result != SQLITE_OK) {
        goto backup_close_and_cleanup;
    }
    result = copy_database(destination, source, 128, 500,
                           maximum_duration_ms, stage, page_count,
                           copied_pages, &temporary_state);
    if (result != SQLITE_OK) {
        goto backup_close_and_cleanup;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
    result = normalize_single_file_database(destination);
    if (result != SQLITE_OK) {
        goto backup_close_and_cleanup;
    }
    *stage = HMJE_SQLITE_OPERATION_STAGE_VERIFY;
    result = verify_database(destination, 1, &problems);
    if (result == SQLITE_OK && problems != 0) {
        result = SQLITE_CORRUPT;
    }

backup_close_and_cleanup:
    close_result = sqlite3_close(destination);
    destination = NULL;
    if (result == SQLITE_OK && close_result != SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_CLOSE;
        result = close_result;
    }

    if (result == SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
        result = remove_temporary_database_files(temporary_path, 0);
    }
    if (result == SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
        result = sync_file_to_stable_storage(temporary_path);
    }
    if (result == SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_PUBLISH;
        result = publish_no_replace(temporary_path, destination_path);
        if (result == SQLITE_OK) {
            published = 1;
            *destination_state = HMJE_SQLITE_DESTINATION_APPLIED;
        }
    }
    if (result == SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
        result = sync_parent_directory(destination_path);
        if (result == SQLITE_OK) {
            *durability_state = HMJE_SQLITE_DURABILITY_CONFIRMED;
        }
    }

    if (result != SQLITE_OK && published == 0) {
        if (remove_temporary_database_files(temporary_path, 1) ==
            SQLITE_OK) {
            *destination_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
        } else {
            *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
            *destination_state = HMJE_SQLITE_DESTINATION_CHECK_REQUIRED;
            result = SQLITE_IOERR;
        }
    }
    free(temporary_path);
    free(destination_path);
    return result;
}

static int restore_database(
    uint64_t source_path_address, uint64_t source_path_length,
    uint64_t destination_path_address, uint64_t destination_path_length,
    int allow_existing_destination,
    uint64_t out_stage_address, uint64_t out_page_count_address,
    uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address) {
    sqlite3 *source = NULL;
    sqlite3 *destination = NULL;
    char *source_path = NULL;
    char *destination_path = NULL;
    char *temporary_path = NULL;
    const char *restore_target = NULL;
    restore_source_snapshot source_snapshot = {
        NULL, -1, {0}, {0}
    };
    int32_t *stage;
    uint64_t *page_count;
    uint64_t *copied_pages;
    int32_t *destination_state;
    int32_t *durability_state;
    int32_t temporary_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
    int32_t *copy_state;
    uint64_t problems = 0;
    int destination_existed = 0;
    int published = 0;
    int source_snapshot_unlinked = 0;
    int result;
    int destination_close_result;
    int source_close_result;
    int snapshot_verify_result;
    int snapshot_close_result;

    result = initialize_operation_outputs(
        out_stage_address, out_page_count_address, out_copied_pages_address,
        out_destination_state_address, out_durability_state_address,
        &stage, &page_count, &copied_pages, &destination_state,
        &durability_state);
    if (result != SQLITE_OK) {
        return result;
    }
    *stage = HMJE_SQLITE_OPERATION_STAGE_PATH;
    result = copy_operational_path(source_path_address, source_path_length,
                                   &source_path);
    if (result != SQLITE_OK) {
        return result;
    }
    result = copy_operational_path(destination_path_address,
                                   destination_path_length,
                                   &destination_path);
    if (result != SQLITE_OK) {
        free(source_path);
        return result;
    }
    if (paths_reference_same_file(source_path, destination_path)) {
        result = SQLITE_MISUSE;
        goto restore_free_paths;
    }
    result = path_entry_exists(destination_path, &destination_existed);
    if (result != SQLITE_OK) {
        goto restore_free_paths;
    }
    if (destination_existed != 0 &&
        allow_existing_destination == 0) {
        result = SQLITE_CONSTRAINT;
        goto restore_free_paths;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_SOURCE_OPEN;
    result = capture_restore_source(
        source_path, destination_path, &source_snapshot);
    if (result != SQLITE_OK) {
        goto restore_free_paths;
    }
    result = sqlite3_open_v2(source_snapshot.path, &source,
                             SQLITE_OPEN_READWRITE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (result != SQLITE_OK) {
        goto restore_close_source;
    }
    if (unlink(source_snapshot.path) != 0) {
        result = SQLITE_IOERR;
        goto restore_close_source;
    }
    source_snapshot_unlinked = 1;
    result = verify_restore_source_snapshot(
        &source_snapshot, (nlink_t)0);
    if (result != SQLITE_OK) {
        goto restore_close_source;
    }
    result = require_single_file_restore_source(source);
    if (result != SQLITE_OK) {
        goto restore_close_source;
    }
    result = sqlite3_exec(source, "PRAGMA query_only=ON", NULL, NULL, NULL);
    if (result != SQLITE_OK) {
        goto restore_close_source;
    }
    *stage = HMJE_SQLITE_OPERATION_STAGE_VERIFY;
    result = verify_database(source, 1, &problems);
    if (result != SQLITE_OK) {
        goto restore_close_source;
    }
    if (problems != 0) {
        result = SQLITE_CORRUPT;
        goto restore_close_source;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_DESTINATION_OPEN;
    if (destination_existed == 0) {
        result = create_sibling_temporary_path(destination_path,
                                               &temporary_path);
        if (result != SQLITE_OK) {
            goto restore_close_source;
        }
        restore_target = temporary_path;
        copy_state = &temporary_state;
    } else {
        restore_target = destination_path;
        copy_state = destination_state;
    }
    result = sqlite3_open_v2(restore_target, &destination,
                             SQLITE_OPEN_READWRITE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (result != SQLITE_OK) {
        if (destination != NULL) {
            (void)sqlite3_close(destination);
        }
        destination = NULL;
        goto restore_close_source;
    }
    if (destination_existed != 0 &&
        databases_reference_same_file(source, destination)) {
        result = SQLITE_MISUSE;
        goto restore_close_both;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
    result = configure_durable_destination(destination);
    if (result != SQLITE_OK) {
        goto restore_close_both;
    }
    if (destination_existed != 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_COPY_PREPARE;
        result = normalize_single_file_database(destination);
        if (result != SQLITE_OK) {
            goto restore_close_both;
        }
        result = acquire_restore_exclusive_lock(destination);
        if (result != SQLITE_OK) {
            goto restore_close_both;
        }
    }
    result = copy_database(destination, source, -1, 0, 0, stage, page_count,
                           copied_pages, copy_state);
    if (result != SQLITE_OK) {
        goto restore_close_both;
    }

    *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
    result = normalize_single_file_database(destination);
    if (result != SQLITE_OK) {
        goto restore_close_both;
    }
    *stage = HMJE_SQLITE_OPERATION_STAGE_VERIFY;
    problems = 0;
    result = verify_database(destination, 0, &problems);
    if (result == SQLITE_OK && problems != 0) {
        result = SQLITE_CORRUPT;
    }

restore_close_both:
    destination_close_result = sqlite3_close(destination);
    destination = NULL;
    if (result == SQLITE_OK && destination_close_result != SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_CLOSE;
        result = destination_close_result;
    }

restore_close_source:
    if (source != NULL) {
        source_close_result = sqlite3_close(source);
        source = NULL;
        if (result == SQLITE_OK && source_close_result != SQLITE_OK) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_CLOSE;
            result = source_close_result;
        }
    }
    if (source_snapshot.descriptor >= 0) {
        snapshot_verify_result = verify_restore_source_snapshot(
            &source_snapshot,
            source_snapshot_unlinked != 0 ? (nlink_t)0 : (nlink_t)1);
        if (snapshot_verify_result != SQLITE_OK) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_VERIFY;
            result = snapshot_verify_result;
        }
        snapshot_close_result = close(source_snapshot.descriptor);
        source_snapshot.descriptor = -1;
        if (result == SQLITE_OK && snapshot_close_result != 0) {
            *stage = HMJE_SQLITE_OPERATION_STAGE_CLOSE;
            result = SQLITE_IOERR;
        }
    }
    if (source_snapshot_unlinked == 0 &&
        source_snapshot.path != NULL &&
        remove_temporary_database_files(source_snapshot.path, 1) !=
            SQLITE_OK) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
        result = SQLITE_IOERR;
    }

    if (result == SQLITE_OK && destination_existed == 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
        result = remove_temporary_database_files(temporary_path, 0);
    }
    if (result == SQLITE_OK && destination_existed != 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
        result = sync_file_to_stable_storage(destination_path);
        if (result == SQLITE_OK) {
            *durability_state = HMJE_SQLITE_DURABILITY_CONFIRMED;
        }
    }
    if (result == SQLITE_OK && destination_existed == 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
        result = sync_file_to_stable_storage(temporary_path);
    }
    if (result == SQLITE_OK && destination_existed == 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_PUBLISH;
        result = publish_no_replace(temporary_path, destination_path);
        if (result == SQLITE_OK) {
            published = 1;
            *destination_state = HMJE_SQLITE_DESTINATION_APPLIED;
        }
    }
    if (result == SQLITE_OK && destination_existed == 0) {
        *stage = HMJE_SQLITE_OPERATION_STAGE_SYNC;
        result = sync_parent_directory(destination_path);
        if (result == SQLITE_OK) {
            *durability_state = HMJE_SQLITE_DURABILITY_CONFIRMED;
        }
    }

    if (result != SQLITE_OK && destination_existed == 0 &&
        temporary_path != NULL && published == 0) {
        if (remove_temporary_database_files(temporary_path, 1) ==
            SQLITE_OK) {
            *destination_state = HMJE_SQLITE_DESTINATION_UNCHANGED;
        } else {
            *stage = HMJE_SQLITE_OPERATION_STAGE_CLEANUP;
            *destination_state = HMJE_SQLITE_DESTINATION_CHECK_REQUIRED;
            result = SQLITE_IOERR;
        }
    }

restore_free_paths:
    free(source_path);
    free(destination_path);
    free(temporary_path);
    free(source_snapshot.path);
    return result;
}

int32_t hmje_sqlite_restore(
    uint64_t source_path_address, uint64_t source_path_length,
    uint64_t destination_path_address, uint64_t destination_path_length,
    uint64_t out_stage_address, uint64_t out_page_count_address,
    uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address) {
    return restore_database(
        source_path_address, source_path_length,
        destination_path_address, destination_path_length, 1,
        out_stage_address, out_page_count_address,
        out_copied_pages_address, out_destination_state_address,
        out_durability_state_address);
}

int32_t hmje_sqlite_restore_new(
    uint64_t source_path_address, uint64_t source_path_length,
    uint64_t destination_path_address, uint64_t destination_path_length,
    uint64_t out_stage_address, uint64_t out_page_count_address,
    uint64_t out_copied_pages_address,
    uint64_t out_destination_state_address,
    uint64_t out_durability_state_address) {
    return restore_database(
        source_path_address, source_path_length,
        destination_path_address, destination_path_length, 0,
        out_stage_address, out_page_count_address,
        out_copied_pages_address, out_destination_state_address,
        out_durability_state_address);
}
