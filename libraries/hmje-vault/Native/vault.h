#ifndef HMJE_VAULT_H
#define HMJE_VAULT_H
#include <stddef.h>
#include <stdint.h>
#define VAULT_NAME_MAX 128
#define VAULT_VALUE_MAX 65536
#define VAULT_PATH_MAX 4096
enum {
    VAULT_OK = 0,
    VAULT_MISSING = 1,
    VAULT_INVALID = 2,
    VAULT_UNSUPPORTED = 3,
    VAULT_LOCKED = 4,
    VAULT_INTEGRITY = 5,
    VAULT_IO = 6,
    VAULT_INTERNAL = 7
};
/* All spans are borrowed for the call. Values may be binary and empty. Names
 * must be nonempty strict UTF-8 without U+0000. No API prints secret bytes. */
int64_t hmje_vault_get(const void *, int64_t, const void *, int64_t, void *, int64_t, uint64_t *);
int64_t hmje_vault_set(const void *, int64_t, const void *, int64_t, const void *, int64_t);
int64_t hmje_vault_delete(const void *, int64_t, const void *, int64_t);
int64_t hmje_vault_app_data_path(void *, int64_t, uint64_t *);
int64_t hmje_vault_utf8_valid(const void *, int64_t, int64_t);
int64_t hmje_vault_wipe(void *, int64_t);
static inline int vault_names(const void *ns, int64_t nl, const void *key, int64_t kl) {
    return nl > 0 && nl <= VAULT_NAME_MAX && kl > 0 && kl <= VAULT_NAME_MAX &&
           hmje_vault_utf8_valid(ns, nl, 0) && hmje_vault_utf8_valid(key, kl, 0);
}
static inline void vault_hex(const unsigned char *src, size_t n, char *dst) {
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        dst[i * 2] = hex[src[i] >> 4];
        dst[i * 2 + 1] = hex[src[i] & 15];
    }
    dst[n * 2] = 0;
}
#endif
