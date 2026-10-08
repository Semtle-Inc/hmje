#include "crypto.h"
#include <sys/random.h>
#include <errno.h>
/* Blocking getrandom(0) waits for kernel CRNG readiness; no clock/PRNG fallback. */
int crypto_random_os(void *out,size_t len){
    unsigned char *p=out;size_t done=0;
    while(done<len){ssize_t n=getrandom(p+done,len-done,0);
        if(n<0){if(errno==EINTR)continue;return CRYPTO_INTERNAL;}
        if(n==0 || (size_t)n>len-done)return CRYPTO_INTERNAL;
        done+=(size_t)n;
    }
    return CRYPTO_OK;
}
