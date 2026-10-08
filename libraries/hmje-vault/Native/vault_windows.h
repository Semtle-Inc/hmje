#ifndef HMJE_VAULT_WINDOWS_H
#define HMJE_VAULT_WINDOWS_H
#include "vault.h"
#include <windows.h>
#include <bcrypt.h>
#define VAULT_WIN_FILE_MAX (VAULT_VALUE_MAX + 36)
typedef struct {
    WCHAR directory[VAULT_PATH_MAX];
    WCHAR file[VAULT_PATH_MAX];
    WCHAR credential[128];
    unsigned char aad[32];
    SECURITY_ATTRIBUTES security;
    PSECURITY_DESCRIPTOR descriptor;
    HANDLE lock;
} vault_win_context;
int vault_win_status(DWORD);
int vault_win_regular(HANDLE);
int vault_win_begin(vault_win_context *, const void *, int64_t, const void *, int64_t);
void vault_win_end(vault_win_context *);
int vault_win_key(vault_win_context *, int, unsigned char[32]);
int vault_win_cipher(int, const unsigned char[32], const unsigned char[32],
                     unsigned char[12], unsigned char[16], const void *, ULONG, void *);
int vault_win_publish(vault_win_context *, const unsigned char *, DWORD, int);
#endif
