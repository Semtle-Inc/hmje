#include "crypto.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
int crypto_random_os(void *out,size_t len){
    /* The common wrapper bounds len to 1 MiB before this ULONG conversion. */
    return BCryptGenRandom(NULL,(PUCHAR)out,(ULONG)len,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0?CRYPTO_OK:CRYPTO_INTERNAL;
}
