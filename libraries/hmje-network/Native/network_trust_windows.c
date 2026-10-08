#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <stdint.h>

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

    WCHAR host[254];
    int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        (const char *)(uintptr_t)host_address,
        (int)host_length,
        host,
        253
    );
    if (converted <= 0) return -1;
    host[converted] = L'\0';

    const hmje_certificate_descriptor *descriptors =
        (const hmje_certificate_descriptor *)(uintptr_t)descriptors_address;
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_MEMORY,
        0,
        0,
        CERT_STORE_CREATE_NEW_FLAG,
        NULL
    );
    PCCERT_CONTEXT leaf = NULL;
    int ok = store != NULL;
    for (int64_t index = 0; ok && index < certificate_count; ++index) {
        if (descriptors[index].address == 0 || descriptors[index].length == 0 ||
            descriptors[index].length > UINT32_MAX) {
            ok = 0;
            break;
        }
        PCCERT_CONTEXT certificate = CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            (const BYTE *)(uintptr_t)descriptors[index].address,
            (DWORD)descriptors[index].length
        );
        if (certificate == NULL || !CertAddCertificateContextToStore(
                store,
                certificate,
                CERT_STORE_ADD_ALWAYS,
                index == 0 ? &leaf : NULL
            )) {
            if (certificate != NULL) CertFreeCertificateContext(certificate);
            ok = 0;
            break;
        }
        CertFreeCertificateContext(certificate);
    }

    CERT_CHAIN_PARA chain_parameters;
    ZeroMemory(&chain_parameters, sizeof(chain_parameters));
    chain_parameters.cbSize = sizeof(chain_parameters);
    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (ok) ok = CertGetCertificateChain(
        NULL,
        leaf,
        NULL,
        store,
        &chain_parameters,
        0,
        NULL,
        &chain
    );
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl;
    ZeroMemory(&ssl, sizeof(ssl));
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = host;
    CERT_CHAIN_POLICY_PARA policy;
    ZeroMemory(&policy, sizeof(policy));
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status;
    ZeroMemory(&status, sizeof(status));
    status.cbSize = sizeof(status);
    if (ok) ok = CertVerifyCertificateChainPolicy(
        CERT_CHAIN_POLICY_SSL,
        chain,
        &policy,
        &status
    ) && status.dwError == 0;

    if (chain != NULL) CertFreeCertificateChain(chain);
    if (leaf != NULL) CertFreeCertificateContext(leaf);
    if (store != NULL) CertCloseStore(store, 0);
    return ok ? 0 : -1;
}

static int hmje_der_length(
    const unsigned char *data,
    size_t available,
    size_t *header_length,
    size_t *value_length
) {
    if (data == NULL || available < 2) return 0;
    if ((data[1] & 0x80) == 0) {
        *header_length = 2;
        *value_length = data[1];
        return *value_length <= available - 2;
    }
    size_t count = data[1] & 0x7f;
    if (count == 0 || count > sizeof(size_t) || 2 + count > available ||
        data[2] == 0) return 0;
    size_t length = 0;
    for (size_t index = 0; index < count; ++index) {
        length = (length << 8) | data[2 + index];
    }
    if (length < 128 || length > available - 2 - count) return 0;
    *header_length = 2 + count;
    *value_length = length;
    return 1;
}

static int hmje_ecdsa_der_to_p1363(
    const unsigned char *signature,
    size_t signature_length,
    unsigned char output[64]
) {
    size_t sequence_header = 0, sequence_length = 0;
    if (signature == NULL || signature_length < 8 || signature[0] != 0x30 ||
        !hmje_der_length(signature, signature_length, &sequence_header, &sequence_length) ||
        sequence_header + sequence_length != signature_length) return 0;
    size_t position = sequence_header;
    for (size_t part = 0; part < 2; ++part) {
        if (position >= signature_length || signature[position] != 0x02) return 0;
        size_t integer_header = 0, integer_length = 0;
        if (!hmje_der_length(
                signature + position,
                signature_length - position,
                &integer_header,
                &integer_length
            ) || integer_length == 0) return 0;
        size_t encoded_length = integer_length;
        const unsigned char *integer = signature + position + integer_header;
        if ((integer[0] & 0x80) != 0) return 0;
        if (integer_length > 1 && integer[0] == 0) {
            ++integer;
            --integer_length;
        }
        if (integer_length > 32) return 0;
        ZeroMemory(output + part * 32, 32);
        CopyMemory(output + part * 32 + (32 - integer_length), integer, integer_length);
        position += integer_header + encoded_length;
    }
    return position == signature_length;
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
        certificate_length > UINT32_MAX || digest_address == 0 ||
        digest_length != 32 || signature_address == 0 ||
        signature_length <= 0 || signature_length > INT32_MAX) return -1;
    PCCERT_CONTEXT certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        (const BYTE *)(uintptr_t)certificate_address,
        (DWORD)certificate_length
    );
    BCRYPT_KEY_HANDLE key = NULL;
    int ok = certificate != NULL && CryptImportPublicKeyInfoEx2(
        X509_ASN_ENCODING,
        &certificate->pCertInfo->SubjectPublicKeyInfo,
        0,
        NULL,
        &key
    );
    unsigned char raw_signature[64];
    if (ok) ok = hmje_ecdsa_der_to_p1363(
        (const unsigned char *)(uintptr_t)signature_address,
        (size_t)signature_length,
        raw_signature
    );
    if (ok) ok = BCryptVerifySignature(
        key,
        NULL,
        (PUCHAR)(uintptr_t)digest_address,
        (ULONG)digest_length,
        raw_signature,
        sizeof(raw_signature),
        0
    ) == 0;
    if (key != NULL) BCryptDestroyKey(key);
    if (certificate != NULL) CertFreeCertificateContext(certificate);
    return ok ? 0 : -1;
}
