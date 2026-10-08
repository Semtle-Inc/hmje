#include "crypto.h"
#include <string.h>
#include <stdlib.h>
int64_t hmje_crypto_wipe(void *p,size_t n){if(n>INT64_MAX || !crypto_span(p,(int64_t)n))return CRYPTO_INVALID;volatile unsigned char *b=p;while(n--)*b++=0;return CRYPTO_OK;}
int crypto_span(const void *p,int64_t n){return n>=0 && (!n || (p && (uint64_t)n<=UINTPTR_MAX-(uintptr_t)p));}
int crypto_overlap(const void *a,size_t an,const void *b,size_t bn){return an && bn && (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an;}
int crypto_public_format(const void *p,int64_t n){return n==65 && crypto_span(p,n) && ((const unsigned char *)p)[0]==4;}
int crypto_private_format(const void *p,int64_t n){
    static const unsigned char order[32]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
    if(n!=97 || !crypto_span(p,n) || !crypto_public_format(p,65))return 0;
    const unsigned char *d=(const unsigned char *)p+65;unsigned any=0;for(int i=0;i<32;i++)any|=d[i];
    return any && memcmp(d,order,32)<0;
}
int crypto_generate_args(void *out,int64_t cap){if(cap<97)return CRYPTO_CAPACITY;return crypto_span(out,cap)?CRYPTO_OK:CRYPTO_INVALID;}
int crypto_agree_args(const void *key,int64_t kl,const void *peer,int64_t pl,void *out,int64_t cap){
    if(!crypto_private_format(key,kl) || !crypto_public_format(peer,pl))return CRYPTO_INVALID;
    if(cap<32)return CRYPTO_CAPACITY;
    if(!crypto_span(out,cap))return CRYPTO_INVALID;
    if(crypto_overlap(out,32,key,97)||crypto_overlap(out,32,peer,65))return CRYPTO_INVALID;
    return CRYPTO_OK;
}

int64_t hmje_crypto_random(void *out,int64_t cap,int64_t len){
    if(len<0 || len>HMJE_CRYPTO_RANDOM_MAX)return CRYPTO_INVALID;
    if(cap<len)return CRYPTO_CAPACITY;
    if(!crypto_span(out,cap))return CRYPTO_INVALID;
    if(!len)return CRYPTO_OK;
    unsigned char *temporary=malloc((size_t)len);
    if(!temporary)return CRYPTO_INTERNAL;
    int result=crypto_random_os(temporary,(size_t)len);
    if(result==CRYPTO_OK)memcpy(out,temporary,(size_t)len);
    hmje_crypto_wipe(temporary,(size_t)len);free(temporary);
    return result;
}
