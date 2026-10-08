#include "vault.h"
int64_t hmje_vault_get(const void *n, int64_t nl, const void *k, int64_t kl, void *o, int64_t c,
                       uint64_t *l) {
    if (l)
        *l = 0;
    return l && vault_names(n, nl, k, kl) && c >= 0 && (!c || o) ? VAULT_UNSUPPORTED
                                                                 : VAULT_INVALID;
}
int64_t hmje_vault_set(const void *n, int64_t nl, const void *k, int64_t kl, const void *v,
                       int64_t s) {
    if (!vault_names(n, nl, k, kl) || s < 0 || s > VAULT_VALUE_MAX || (s && !v))
        return VAULT_INVALID;
    return VAULT_UNSUPPORTED;
}
int64_t hmje_vault_delete(const void *n, int64_t nl, const void *k, int64_t kl) {
    return vault_names(n, nl, k, kl) ? VAULT_UNSUPPORTED : VAULT_INVALID;
}
int64_t hmje_vault_app_data_path(void *o, int64_t c, uint64_t *l) {
    if (l)
        *l = 0;
    return o && c > 0 && l ? VAULT_UNSUPPORTED : VAULT_INVALID;
}
