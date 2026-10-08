#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <sys/random.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#include <unistd.h>
#endif

int64_t hmje_network_secure_random(int64_t output, int64_t length) {
    if (output == 0 || length <= 0) return -1;
    uint8_t *cursor = (uint8_t *)(uintptr_t)output;
    size_t remaining = (size_t)length;
    while (remaining != 0) {
#ifdef __ANDROID__
        /* Bionic exposes getrandom() only from API 28; Android 26/27 still
         * provide the kernel CRNG syscall. Keep entropy blocking and fail closed. */
        ssize_t count = syscall(__NR_getrandom, cursor, remaining, 0);
#else
        ssize_t count = getrandom(cursor, remaining, 0);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        cursor += (size_t)count;
        remaining -= (size_t)count;
    }
    return 0;
}
