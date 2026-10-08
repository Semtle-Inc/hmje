#include <stdint.h>

int64_t hmje_network_verify_server_trust(
    int64_t host_address,
    int64_t host_length,
    int64_t descriptors_address,
    int64_t certificate_count
) {
    (void)host_address;
    (void)host_length;
    (void)descriptors_address;
    (void)certificate_count;
    return -1;
}

int64_t hmje_network_verify_server_ecdsa_p256_sha256(
    int64_t certificate_address,
    int64_t certificate_length,
    int64_t digest_address,
    int64_t digest_length,
    int64_t signature_address,
    int64_t signature_length
) {
    (void)certificate_address;
    (void)certificate_length;
    (void)digest_address;
    (void)digest_length;
    (void)signature_address;
    (void)signature_length;
    return -1;
}
