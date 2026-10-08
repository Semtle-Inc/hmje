#include "vault_windows.h"
#include <wincred.h>
#include <stdio.h>
#include <string.h>

/* Credential Manager protects only the namespace key; authenticated ciphertext
 * files hold the full 64 KiB value range. LOCAL_MACHINE here means persistence
 * across this user's local logon sessions, not access for all machine users. */
int vault_win_key(vault_win_context *context, int create, unsigned char key[32]) {
    PCREDENTIALW credential = NULL;
    if (CredReadW(context->credential, CRED_TYPE_GENERIC, 0, &credential)) {
        int result = VAULT_INTEGRITY;
        if (credential && credential->Type == CRED_TYPE_GENERIC &&
            credential->CredentialBlobSize == 32 && credential->CredentialBlob) {
            memcpy(key, credential->CredentialBlob, 32);
            result = VAULT_OK;
        }
        if (credential) {
            if (credential->CredentialBlob)
                hmje_vault_wipe(credential->CredentialBlob, credential->CredentialBlobSize);
            CredFree(credential);
        }
        return result;
    }
    DWORD error = GetLastError();
    if (error != ERROR_NOT_FOUND) return vault_win_status(error);
    if (!create) return VAULT_INTEGRITY;
    /* Lost protection must not silently rotate the key of an existing namespace. */
    WCHAR pattern[VAULT_PATH_MAX];
    WIN32_FIND_DATAW found;
    swprintf_s(pattern, VAULT_PATH_MAX, L"%s\\*.bin", context->directory);
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) { FindClose(search); return VAULT_INTEGRITY; }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return VAULT_IO;
    if (BCryptGenRandom(NULL, key, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return VAULT_INTERNAL;
    CREDENTIALW new_key = {0};
    new_key.Type = CRED_TYPE_GENERIC;
    new_key.TargetName = context->credential;
    new_key.CredentialBlob = key;
    new_key.CredentialBlobSize = 32;
    new_key.Persist = CRED_PERSIST_LOCAL_MACHINE;
    new_key.UserName = L"hmje-vault";
    if (!CredWriteW(&new_key, 0)) return vault_win_status(GetLastError());
    return VAULT_OK;
}

int vault_win_cipher(int encrypt, const unsigned char key[32], const unsigned char aad[32],
                     unsigned char nonce[12], unsigned char tag[16],
                     const void *input, ULONG length, void *output) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE handle = NULL;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    unsigned char empty = 0;
    ULONG written = 0;
    int result = VAULT_INTERNAL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) goto done;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) goto done;
    if (BCryptGenerateSymmetricKey(algorithm, &handle, NULL, 0, (PUCHAR)key, 32, 0) < 0) goto done;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = 32;
    info.pbTag = tag;
    info.cbTag = 16;
    NTSTATUS status = encrypt
        ? BCryptEncrypt(handle, length ? (PUCHAR)input : &empty, length, &info, NULL, 0,
                         output, length, &written, 0)
        : BCryptDecrypt(handle, length ? (PUCHAR)input : &empty, length, &info, NULL, 0,
                         output, length, &written, 0);
    if (status >= 0 && written == length) result = VAULT_OK;
    else if (!encrypt && status == (NTSTATUS)0xc000a002L) result = VAULT_INTEGRITY;
done:
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
