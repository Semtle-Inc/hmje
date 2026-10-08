#include "windows_host_path.h"

#if defined(_WIN32)
#include <bcrypt.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

struct hmje_sha256_windows {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
};

static int hmje_sha256_begin(struct hmje_sha256_windows *state) {
    memset(state, 0, sizeof(*state));
    if (BCryptOpenAlgorithmProvider(&state->algorithm, BCRYPT_SHA256_ALGORITHM,
                                    NULL, 0) < 0) return 0;
    if (BCryptCreateHash(state->algorithm, &state->hash, NULL, 0,
                         NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(state->algorithm, 0);
        state->algorithm = NULL;
        return 0;
    }
    return 1;
}

static void hmje_sha256_end(struct hmje_sha256_windows *state) {
    if (state->hash) BCryptDestroyHash(state->hash);
    if (state->algorithm) BCryptCloseAlgorithmProvider(state->algorithm, 0);
}

static int hmje_sha256_update(struct hmje_sha256_windows *state,
                              const unsigned char *bytes, uint64_t length) {
    uint64_t offset = 0;
    while (offset < length) {
        ULONG count = length - offset > 65536 ? 65536 : (ULONG)(length - offset);
        if (BCryptHashData(state->hash, (PUCHAR)(bytes + offset), count, 0) < 0)
            return 0;
        offset += count;
    }
    return 1;
}

static int hmje_sha256_finish(struct hmje_sha256_windows *state, char result[65]) {
    unsigned char digest[32];
    if (BCryptFinishHash(state->hash, digest, sizeof(digest), 0) < 0) return 0;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    result[64] = 0;
    return 1;
}

int64_t hmje_compiler_bytes_sha256(uint64_t input_address, uint64_t input_length,
                                   uint64_t output_address, uint64_t capacity) {
    if ((!input_address && input_length) || input_length > SIZE_MAX ||
        !output_address || capacity < 65) return -EINVAL;
    struct hmje_sha256_windows state;
    if (!hmje_sha256_begin(&state)) return -EIO;
    int success = hmje_sha256_update(&state,
        (const unsigned char *)(uintptr_t)input_address, input_length);
    char result[65];
    if (success) success = hmje_sha256_finish(&state, result);
    hmje_sha256_end(&state);
    if (!success) return -EIO;
    memcpy((void *)(uintptr_t)output_address, result, sizeof(result));
    return 0;
}

int64_t hmje_compiler_file_sha256(uint64_t path_address, uint64_t path_length,
                                  uint64_t output_address, uint64_t capacity) {
    if (!output_address || capacity < 65) return -EINVAL;
    wchar_t *path = hmje_windows_host_path(path_address, path_length);
    if (!path) return -EINVAL;
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return -ENOENT;
    struct hmje_sha256_windows state;
    if (!hmje_sha256_begin(&state)) { CloseHandle(file); return -EIO; }
    unsigned char bytes[65536];
    int success = 1;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, bytes, sizeof(bytes), &count, NULL)) { success = 0; break; }
        if (!count) break;
        if (!hmje_sha256_update(&state, bytes, count)) { success = 0; break; }
    }
    if (!CloseHandle(file)) success = 0;
    char result[65];
    if (success) success = hmje_sha256_finish(&state, result);
    hmje_sha256_end(&state);
    if (!success) return -EIO;
    memcpy((void *)(uintptr_t)output_address, result, sizeof(result));
    return 0;
}
#endif
