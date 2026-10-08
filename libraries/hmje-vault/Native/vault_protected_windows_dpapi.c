#include "vault_protected_windows.h"
#include <dpapi.h>
#include <stdlib.h>
#include <string.h>

/* OS boundary. The OS allocates DPAPI output; every plaintext DATA_BLOB is
 * wiped before LocalFree, including failed/oversized output. No machine scope. */
int64_t vault_win_protected_blob(uint64_t op, const void *ns, int64_t nl,
    const void *key, int64_t kl, void *data, int64_t capacity, uint64_t *length, int add_only) {
    if (length) *length = 0;
    if (!length || !vault_names(ns,nl,key,kl) || capacity < 0 ||
        capacity > VP_WIN_PAYLOAD_MAX || (capacity && !data) || op > 3) return VAULT_INVALID;
#ifdef HMJE_VAULT_WINDOWS_DPAPI_TEST
    extern int64_t hmje_vp_win_test_blob_fault(uint64_t);
    int64_t injected=hmje_vp_win_test_blob_fault(op);
    if (injected) return injected;
#endif
    vault_win_context context;
    int64_t result = vault_win_begin(&context,ns,nl,key,kl);
    /* Plain storage calls missing app identity/impersonation "unsupported".
     * Protected storage reserves that result for Hello availability. */
    if (result) return result==VAULT_UNSUPPORTED ? VAULT_INVALID : result;
    if (wcscat_s(context.file,VAULT_PATH_MAX,L".presence.dpapi")) {
        vault_win_end(&context); return VAULT_IO;
    }
    DATA_BLOB encrypted = {0}, plain = {0};
    DATA_BLOB entropy = {sizeof(context.aad),context.aad};
    HANDLE file = INVALID_HANDLE_VALUE;
    if (op == 1) {
        plain.cbData = (DWORD)capacity; plain.pbData = data;
        if (!CryptProtectData(&plain,NULL,&entropy,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)) {
            result = vault_win_status(GetLastError()); goto done;
        }
        result = vault_win_publish(&context,encrypted.pbData,encrypted.cbData,add_only);
        goto done;
    }
    DWORD attributes = GetFileAttributesW(context.file);
    if (attributes == INVALID_FILE_ATTRIBUTES) { result = vault_win_status(GetLastError()); goto done; }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        result = VAULT_INTEGRITY; goto done;
    }
    if (op == 0) goto done; /* existence: metadata only */
    if (op == 3) {
        if (!DeleteFileW(context.file)) result = vault_win_status(GetLastError());
        goto done;
    }
    file = CreateFileW(context.file,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (file == INVALID_HANDLE_VALUE) { result = vault_win_status(GetLastError()); goto done; }
    LARGE_INTEGER size;
    if (!vault_win_regular(file) || !GetFileSizeEx(file,&size) || size.QuadPart <= 0 || size.QuadPart > 1024*1024) {
        result = VAULT_INTEGRITY; goto done;
    }
    encrypted.cbData = (DWORD)size.QuadPart;
    encrypted.pbData = LocalAlloc(LMEM_FIXED,encrypted.cbData);
    if (!encrypted.pbData) { result = VAULT_INTERNAL; goto done; }
    DWORD position = 0;
    while (position < encrypted.cbData) {
        DWORD count = 0;
        if (!ReadFile(file,encrypted.pbData+position,encrypted.cbData-position,&count,NULL)) {
            result = vault_win_status(GetLastError()); goto done;
        }
        if (!count) { result = VAULT_INTEGRITY; goto done; }
        position += count;
    }
    if (!CryptUnprotectData(&encrypted,NULL,&entropy,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain)) {
        DWORD error = GetLastError();
        result = error == ERROR_INVALID_DATA ? VAULT_INTEGRITY : vault_win_status(error);
        goto done;
    }
    if (plain.cbData > VP_WIN_PAYLOAD_MAX) { result = VAULT_INTEGRITY; goto done; }
    if (plain.cbData > (uint64_t)capacity) { result = VAULT_INVALID; goto done; }
    if (plain.cbData) memcpy(data,plain.pbData,plain.cbData);
    *length = plain.cbData;
#ifdef HMJE_VAULT_WINDOWS_DPAPI_TEST
    extern int64_t hmje_vp_win_test_readback(void);
    if (plain.cbData > 1 && hmje_vp_win_test_readback()) ((unsigned char *)data)[1] ^= 1;
#endif
done:
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && !result) result = VAULT_IO;
    if (op != 1 && plain.pbData) { SecureZeroMemory(plain.pbData,plain.cbData); LocalFree(plain.pbData); }
    if (encrypted.pbData) LocalFree(encrypted.pbData);
    vault_win_end(&context);
    if (result) *length = 0;
    return result;
}
