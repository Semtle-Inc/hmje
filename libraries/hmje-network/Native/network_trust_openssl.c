#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t address;
    uint64_t length;
} hmje_certificate_descriptor;

int64_t hmje_network_verify_server_trust(
    int64_t host_address,
    int64_t host_length,
    int64_t descriptors_address,
    int64_t certificate_count
) {
    if (host_address == 0 || host_length <= 0 || host_length > 253 ||
        descriptors_address == 0 || certificate_count <= 0 ||
        certificate_count > 16) return -1;

    char host[254];
    const char *source_host = (const char *)(uintptr_t)host_address;
    for (int64_t index = 0; index < host_length; ++index) {
        if (source_host[index] == '\0') return -1;
        host[index] = source_host[index];
    }
    host[host_length] = '\0';

    const hmje_certificate_descriptor *descriptors =
        (const hmje_certificate_descriptor *)(uintptr_t)descriptors_address;
    X509 *leaf = NULL;
    STACK_OF(X509) *untrusted = sk_X509_new_null();
    X509_STORE *store = X509_STORE_new();
    X509_STORE_CTX *context = X509_STORE_CTX_new();
    int ok = untrusted != NULL && store != NULL && context != NULL;
    for (int64_t index = 0; ok && index < certificate_count; ++index) {
        if (descriptors[index].address == 0 || descriptors[index].length == 0 ||
            descriptors[index].length > INT32_MAX) {
            ok = 0;
            break;
        }
        const unsigned char *cursor =
            (const unsigned char *)(uintptr_t)descriptors[index].address;
        X509 *certificate = d2i_X509(
            NULL,
            &cursor,
            (long)descriptors[index].length
        );
        if (certificate == NULL) {
            ok = 0;
            break;
        }
        if (index == 0) leaf = certificate;
        else if (!sk_X509_push(untrusted, certificate)) {
            X509_free(certificate);
            ok = 0;
        }
    }
    if (ok) ok = X509_STORE_set_default_paths(store) == 1;
    if (ok) ok = X509_STORE_CTX_init(context, store, leaf, untrusted) == 1;
    if (ok) {
        X509_VERIFY_PARAM *parameters = X509_STORE_CTX_get0_param(context);
        ok = parameters != NULL &&
            X509_VERIFY_PARAM_set1_host(parameters, host, (size_t)host_length) == 1;
    }
    if (ok) ok = X509_verify_cert(context) == 1;

    X509_STORE_CTX_free(context);
    X509_STORE_free(store);
    if (leaf != NULL) X509_free(leaf);
    if (untrusted != NULL) sk_X509_pop_free(untrusted, X509_free);
    return ok ? 0 : -1;
}

int64_t hmje_network_verify_server_ecdsa_p256_sha256(
    int64_t certificate_address,
    int64_t certificate_length,
    int64_t digest_address,
    int64_t digest_length,
    int64_t signature_address,
    int64_t signature_length
) {
    if (certificate_address == 0 || certificate_length <= 0 ||
        certificate_length > INT32_MAX || digest_address == 0 ||
        digest_length != 32 || signature_address == 0 ||
        signature_length <= 0 || signature_length > INT32_MAX) return -1;

    const unsigned char *cursor =
        (const unsigned char *)(uintptr_t)certificate_address;
    X509 *certificate = d2i_X509(NULL, &cursor, (long)certificate_length);
    EVP_PKEY *key = certificate == NULL ? NULL : X509_get_pubkey(certificate);
    EVP_PKEY_CTX *context = key == NULL ? NULL : EVP_PKEY_CTX_new(key, NULL);
    int ok = key != NULL && EVP_PKEY_base_id(key) == EVP_PKEY_EC &&
        context != NULL && EVP_PKEY_verify_init(context) > 0 &&
        EVP_PKEY_CTX_set_signature_md(context, EVP_sha256()) > 0 &&
        EVP_PKEY_verify(
            context,
            (const unsigned char *)(uintptr_t)signature_address,
            (size_t)signature_length,
            (const unsigned char *)(uintptr_t)digest_address,
            (size_t)digest_length
        ) == 1;

    if (context != NULL) EVP_PKEY_CTX_free(context);
    if (key != NULL) EVP_PKEY_free(key);
    if (certificate != NULL) X509_free(certificate);
    return ok ? 0 : -1;
}
