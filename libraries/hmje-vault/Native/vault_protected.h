#ifndef HMJE_VAULT_PROTECTED_H
#define HMJE_VAULT_PROTECTED_H
#include "vault.h"
/* OS bridge only. Handles are monotonically assigned, never pointer casts.
 * Start copies borrowed spans before returning. Poll copies into caller storage.
 * All requests (including terminal ones) must be closed to wipe retained data. */
uint64_t hmje_vault_protected_start(uint64_t, const void *, int64_t, const void *, int64_t,
    uint64_t, const void *, int64_t, const void *, int64_t, uint64_t *);
int64_t hmje_vault_protected_exists(const void *, int64_t, const void *, int64_t);
int64_t hmje_vault_protected_poll(uint64_t, void *, int64_t, uint64_t *);
int64_t hmje_vault_protected_cancel(uint64_t);
int64_t hmje_vault_protected_close(uint64_t);
uint64_t hmje_vault_protected_migration_ready(uint64_t);
int64_t hmje_vault_protected_transfer(uint64_t, uint64_t, void *, int64_t, uint64_t *);
int64_t hmje_vault_protected_finish(uint64_t, uint64_t);
int64_t hmje_vault_protected_availability(uint64_t *);
#endif
