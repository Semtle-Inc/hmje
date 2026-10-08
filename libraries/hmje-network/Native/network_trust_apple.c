#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
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
    if (host_address == 0 || host_length <= 0 || descriptors_address == 0 ||
        certificate_count <= 0 || certificate_count > 16) return -1;

    CFStringRef host = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        (const UInt8 *)(uintptr_t)host_address,
        (CFIndex)host_length,
        kCFStringEncodingUTF8,
        false
    );
    if (host == NULL) return -1;
    CFMutableArrayRef certificates = CFArrayCreateMutable(
        kCFAllocatorDefault,
        (CFIndex)certificate_count,
        &kCFTypeArrayCallBacks
    );
    if (certificates == NULL) {
        CFRelease(host);
        return -1;
    }

    const hmje_certificate_descriptor *descriptors =
        (const hmje_certificate_descriptor *)(uintptr_t)descriptors_address;
    int ok = 1;
    for (int64_t index = 0; index < certificate_count; ++index) {
        if (descriptors[index].address == 0 || descriptors[index].length == 0 ||
            descriptors[index].length > INT32_MAX) {
            ok = 0;
            break;
        }
        CFDataRef data = CFDataCreate(
            kCFAllocatorDefault,
            (const UInt8 *)(uintptr_t)descriptors[index].address,
            (CFIndex)descriptors[index].length
        );
        SecCertificateRef certificate = data == NULL
            ? NULL
            : SecCertificateCreateWithData(kCFAllocatorDefault, data);
        if (data != NULL) CFRelease(data);
        if (certificate == NULL) {
            ok = 0;
            break;
        }
        CFArrayAppendValue(certificates, certificate);
        CFRelease(certificate);
    }

    SecPolicyRef policy = ok ? SecPolicyCreateSSL(true, host) : NULL;
    SecTrustRef trust = NULL;
    if (policy == NULL || SecTrustCreateWithCertificates(
            certificates,
            policy,
            &trust
        ) != errSecSuccess) {
        ok = 0;
    }
    if (ok) ok = SecTrustEvaluateWithError(trust, NULL);

    if (trust != NULL) CFRelease(trust);
    if (policy != NULL) CFRelease(policy);
    CFRelease(certificates);
    CFRelease(host);
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

    CFDataRef certificate_data = CFDataCreate(
        kCFAllocatorDefault,
        (const UInt8 *)(uintptr_t)certificate_address,
        (CFIndex)certificate_length
    );
    SecCertificateRef certificate = certificate_data == NULL
        ? NULL
        : SecCertificateCreateWithData(kCFAllocatorDefault, certificate_data);
    SecKeyRef key = certificate == NULL ? NULL : SecCertificateCopyKey(certificate);
    CFDataRef digest = CFDataCreate(
        kCFAllocatorDefault,
        (const UInt8 *)(uintptr_t)digest_address,
        (CFIndex)digest_length
    );
    CFDataRef signature = CFDataCreate(
        kCFAllocatorDefault,
        (const UInt8 *)(uintptr_t)signature_address,
        (CFIndex)signature_length
    );
    int ok = key != NULL && digest != NULL && signature != NULL &&
        SecKeyIsAlgorithmSupported(
            key,
            kSecKeyOperationTypeVerify,
            kSecKeyAlgorithmECDSASignatureDigestX962SHA256
        ) && SecKeyVerifySignature(
            key,
            kSecKeyAlgorithmECDSASignatureDigestX962SHA256,
            digest,
            signature,
            NULL
        );

    if (signature != NULL) CFRelease(signature);
    if (digest != NULL) CFRelease(digest);
    if (key != NULL) CFRelease(key);
    if (certificate != NULL) CFRelease(certificate);
    if (certificate_data != NULL) CFRelease(certificate_data);
    return ok ? 0 : -1;
}
