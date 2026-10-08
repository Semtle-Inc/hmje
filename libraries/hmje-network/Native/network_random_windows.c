#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>

int64_t hmje_network_secure_random(int64_t output, int64_t length) {
    if (output == 0 || length <= 0 || (uint64_t)length > UINT32_MAX) return -1;
    NTSTATUS status = BCryptGenRandom(
        NULL,
        (PUCHAR)(uintptr_t)output,
        (ULONG)length,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    return BCRYPT_SUCCESS(status) ? 0 : -1;
}
