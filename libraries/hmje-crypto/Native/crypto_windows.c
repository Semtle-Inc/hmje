#include "crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <string.h>

/* Per-call CNG handles, no persisted keys or mutable shared provider state.
 * The public ABI stays X9.63: 04 || X32 || Y32 [|| private scalar32]. */
typedef struct {
    BCRYPT_ECCKEY_BLOB header;
    unsigned char coordinates[96];
} crypto_ecc_blob;

static BCRYPT_KEY_HANDLE import_key(BCRYPT_ALG_HANDLE algorithm,
                                   const unsigned char *bytes,
                                   int private_key, int signing) {
    crypto_ecc_blob blob = {0};
    BCRYPT_KEY_HANDLE key = NULL;
    blob.header.cbKey = 32;
    blob.header.dwMagic = signing
        ? (private_key ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC)
        : (private_key ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC);
    memcpy(blob.coordinates, bytes + 1, private_key ? 96 : 64);
    NTSTATUS status = BCryptImportKeyPair(algorithm, NULL,
        private_key ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB,
        &key, (PUCHAR)&blob, (ULONG)(sizeof(blob.header) + (private_key ? 96 : 64)), 0);
    hmje_crypto_wipe(&blob, sizeof(blob));
    if (status < 0) {
        if (key) BCryptDestroyKey(key);
        return NULL;
    }
    return key;
}

/* Import alone must not establish public/scalar correspondence. Like the Apple
 * adapter, use an OS ECDSA sign/verify proof over a fixed local-only digest. The
 * signature is never returned or used to authenticate application messages. */
static int pair_valid(const unsigned char *bytes) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE private_key = NULL, public_key = NULL;
    unsigned char digest[32] = {0x48, 0x4d, 0x4a, 0x45, 0x2d, 0x50, 0x32, 0x35, 0x36};
    unsigned char signature[64] = {0};
    ULONG written = 0;
    int valid = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) goto done;
    private_key = import_key(algorithm, bytes, 1, 1);
    public_key = import_key(algorithm, bytes, 0, 1);
    if (!private_key || !public_key) goto done;
    if (BCryptSignHash(private_key, NULL, digest, sizeof(digest), signature,
                       sizeof(signature), &written, 0) < 0 || written != sizeof(signature)) goto done;
    valid = BCryptVerifySignature(public_key, NULL, digest, sizeof(digest),
                                   signature, written, 0) >= 0;
done:
    hmje_crypto_wipe(signature, sizeof(signature));
    if (public_key) BCryptDestroyKey(public_key);
    if (private_key) BCryptDestroyKey(private_key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
}

int64_t hmje_crypto_p256_public(const void *bytes, int64_t length) {
    if (!crypto_public_format(bytes, length)) return CRYPTO_INVALID;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_P256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) return CRYPTO_INTERNAL;
    BCRYPT_KEY_HANDLE key = import_key(algorithm, bytes, 0, 0);
    int result = key ? CRYPTO_OK : CRYPTO_KEY_INVALID;
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

int64_t hmje_crypto_p256_pair(const void *bytes, int64_t length) {
    if (!crypto_private_format(bytes, length)) return CRYPTO_INVALID;
    return pair_valid(bytes) ? CRYPTO_OK : CRYPTO_KEY_INVALID;
}

int64_t hmje_crypto_p256_generate(void *out, int64_t capacity) {
    int result = crypto_generate_args(out, capacity);
    if (result) return result;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    crypto_ecc_blob blob = {0};
    unsigned char encoded[97] = {4};
    ULONG written = 0;
    result = CRYPTO_INTERNAL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_P256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) goto done;
    if (BCryptGenerateKeyPair(algorithm, &key, 256, 0) < 0) goto done;
    if (BCryptFinalizeKeyPair(key, 0) < 0) goto done;
    if (BCryptExportKey(key, NULL, BCRYPT_ECCPRIVATE_BLOB, (PUCHAR)&blob,
                        sizeof(blob), &written, 0) < 0) goto done;
    if (written != sizeof(blob) || blob.header.cbKey != 32 ||
        blob.header.dwMagic != BCRYPT_ECDH_PRIVATE_P256_MAGIC) goto done;
    memcpy(encoded + 1, blob.coordinates, 96);
    if (!crypto_private_format(encoded, sizeof(encoded)) || !pair_valid(encoded)) goto done;
    memcpy(out, encoded, sizeof(encoded));
    result = CRYPTO_OK;
done:
    hmje_crypto_wipe(encoded, sizeof(encoded));
    hmje_crypto_wipe(&blob, sizeof(blob));
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

int64_t hmje_crypto_p256_agree(const void *bytes, int64_t length,
                             const void *peer, int64_t peer_length,
                             void *out, int64_t capacity) {
    int result = crypto_agree_args(bytes, length, peer, peer_length, out, capacity);
    if (result) return result;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE private_key = NULL, public_key = NULL;
    BCRYPT_SECRET_HANDLE secret = NULL;
    unsigned char little_endian[32] = {0}, big_endian[32] = {0};
    ULONG written = 0;
    result = CRYPTO_KEY_INVALID;
    if (!pair_valid(bytes)) goto done;
    result = CRYPTO_INTERNAL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_P256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) goto done;
    result = CRYPTO_KEY_INVALID;
    private_key = import_key(algorithm, bytes, 1, 0);
    public_key = import_key(algorithm, peer, 0, 0);
    if (!private_key || !public_key) goto done;
    result = CRYPTO_INTERNAL;
    if (BCryptSecretAgreement(private_key, public_key, &secret, 0) < 0) goto done;
    /* CNG RAW_SECRET is little-endian; WebCrypto/Security use big-endian X.
     * Keep HKDF outside this adapter to preserve the caller's protocol contract.
     * This module targets Windows 10 and newer. */
    if (BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, NULL, little_endian,
                        sizeof(little_endian), &written, 0) < 0 || written != 32) goto done;
    for (size_t index = 0; index < 32; ++index) big_endian[index] = little_endian[31 - index];
    memcpy(out, big_endian, sizeof(big_endian));
    result = CRYPTO_OK;
done:
    hmje_crypto_wipe(little_endian, sizeof(little_endian));
    hmje_crypto_wipe(big_endian, sizeof(big_endian));
    if (secret) BCryptDestroySecret(secret);
    if (public_key) BCryptDestroyKey(public_key);
    if (private_key) BCryptDestroyKey(private_key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
