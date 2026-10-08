#include <Security/Security.h>
#include <stdint.h>

int64_t hmje_network_secure_random(int64_t output, int64_t length) {
    if (output == 0 || length <= 0) return -1;
    return SecRandomCopyBytes(
        kSecRandomDefault,
        (size_t)length,
        (uint8_t *)(uintptr_t)output
    ) == errSecSuccess ? 0 : -1;
}
