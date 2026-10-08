#include "crypto.h"
int64_t hmje_crypto_p256_public(const void*p,int64_t n){return crypto_public_format(p,n)?CRYPTO_UNSUPPORTED:CRYPTO_INVALID;}
int64_t hmje_crypto_p256_pair(const void*p,int64_t n){return crypto_private_format(p,n)?CRYPTO_UNSUPPORTED:CRYPTO_INVALID;}
int64_t hmje_crypto_p256_generate(void*p,int64_t n){int r=crypto_generate_args(p,n);return r?r:CRYPTO_UNSUPPORTED;}
int64_t hmje_crypto_p256_agree(const void*p,int64_t n,const void*q,int64_t m,void*out,int64_t cap){int r=crypto_agree_args(p,n,q,m,out,cap);return r?r:CRYPTO_UNSUPPORTED;}
