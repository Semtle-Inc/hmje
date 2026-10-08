#include "crypto.h"
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonCryptoError.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

int64_t hmje_crypto_pbkdf2_sha256(const void *password, int64_t password_length,
                                  const void *salt, int64_t salt_length,
                                  int64_t rounds, void *output, int64_t capacity, int64_t length) {
    if (password_length > HMJE_CRYPTO_KDF_MAX || salt_length > HMJE_CRYPTO_KDF_MAX ||
        !crypto_span(password, password_length) || !crypto_span(salt, salt_length) ||
        rounds < 1 || (uint64_t)rounds > UINT32_MAX || length < 1 ||
        length > HMJE_CRYPTO_KDF_MAX || capacity < 0) return CRYPTO_INVALID;
    if (capacity < length) return CRYPTO_CAPACITY;
    if (!crypto_span(output, capacity) ||
        crypto_overlap(output, (size_t)length, password, (size_t)password_length) ||
        crypto_overlap(output, (size_t)length, salt, (size_t)salt_length)) return CRYPTO_INVALID;
#if defined(__APPLE__) || defined(_WIN32)
    unsigned char *temporary = malloc((size_t)length);
    if (!temporary) return CRYPTO_INTERNAL;
    const unsigned char empty = 0;
    if (!password) password = &empty;
    if (!salt) salt = &empty;
    int result = CRYPTO_INTERNAL;
#if defined(__APPLE__)
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password, (size_t)password_length, salt,
            (size_t)salt_length, kCCPRFHmacAlgSHA256, (unsigned)rounds, temporary,
            (size_t)length) == kCCSuccess) result = CRYPTO_OK;
#else
    BCRYPT_ALG_HANDLE provider = NULL;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0) {
        if (BCryptDeriveKeyPBKDF2(provider, (PUCHAR)password, (ULONG)password_length,
                (PUCHAR)salt, (ULONG)salt_length, (ULONGLONG)rounds, temporary,
                (ULONG)length, 0) >= 0) result = CRYPTO_OK;
        BCryptCloseAlgorithmProvider(provider, 0);
    }
#endif
    if (result == CRYPTO_OK) memcpy(output, temporary, (size_t)length);
    hmje_crypto_wipe(temporary, (size_t)length);
    free(temporary);
    return result;
#else
    return CRYPTO_UNSUPPORTED;
#endif
}
