#ifndef HMJE_VAULT_PROTECTED_WINDOWS_H
#define HMJE_VAULT_PROTECTED_WINDOWS_H
#include "vault_protected.h"
#include "vault_windows.h"
/* Payload framing/validation is HMJE's responsibility; C only borrows opaque
 * DPAPI payload spans, DATA_BLOBs, Windows handles and COM ABI interfaces. */
#define VP_WIN_PAYLOAD_MAX (VAULT_VALUE_MAX + 1)
int64_t vault_win_protected_blob(uint64_t operation, const void *ns, int64_t nl,
    const void *key, int64_t kl, void *data, int64_t capacity, uint64_t *length, int add_only);
#endif
