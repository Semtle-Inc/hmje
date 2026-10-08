#ifndef HMJE_CRYPTO_H
#define HMJE_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
enum { CRYPTO_OK=0, CRYPTO_INVALID=1, CRYPTO_CAPACITY=2, CRYPTO_UNSUPPORTED=3,
       CRYPTO_KEY_INVALID=4, CRYPTO_INTERNAL=5 };
/* Borrowed spans, synchronous calls, no persistence/logging. X9.63 public65 and
 * private97=public65||big-endian scalar32. Failure never changes output. */
int64_t hmje_crypto_p256_public(const void *,int64_t);
int64_t hmje_crypto_p256_pair(const void *,int64_t);
int64_t hmje_crypto_p256_generate(void *,int64_t);
int64_t hmje_crypto_p256_agree(const void *,int64_t,const void *,int64_t,void *,int64_t);
/* Transactional OS CSPRNG: 0..1 MiB per call; failure preserves caller bytes. */
#define HMJE_CRYPTO_RANDOM_MAX 1048576
int64_t hmje_crypto_random(void *,int64_t,int64_t);
int crypto_random_os(void *,size_t);
int64_t hmje_crypto_wipe(void *,size_t);
int crypto_span(const void *,int64_t);
int crypto_overlap(const void *,size_t,const void *,size_t);
int crypto_public_format(const void *,int64_t);
int crypto_private_format(const void *,int64_t);
int crypto_agree_args(const void *,int64_t,const void *,int64_t,void *,int64_t);
int crypto_generate_args(void *,int64_t);
/* PBKDF2-HMAC-SHA256, byte-exact input, synchronous. Desktop OS providers.
 * 1..UINT32_MAX rounds, 1..1 MiB output; no implicit password encoding. */
#define HMJE_CRYPTO_KDF_MAX 1048576
int64_t hmje_crypto_pbkdf2_sha256(const void *, int64_t, const void *, int64_t,
                                  int64_t, void *, int64_t, int64_t);
#endif
