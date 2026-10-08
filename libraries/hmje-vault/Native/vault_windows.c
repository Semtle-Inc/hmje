#include "vault_windows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char magic[8] = {'H', 'M', 'J', 'W', 1, 12, 0, 0};

static int write_all(HANDLE file, const void *data, DWORD size) {
    const unsigned char *bytes = data;
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, size, &written, NULL) || written == 0 || written > size) return 0;
        bytes += written;
        size -= written;
    }
    return 1;
}

static int read_cipher(vault_win_context *context, unsigned char **bytes, DWORD *size) {
    *bytes = NULL;
    *size = 0;
    HANDLE file = CreateFileW(context->file, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return vault_win_status(GetLastError());
    LARGE_INTEGER length;
    int result = VAULT_INTEGRITY;
    if (!vault_win_regular(file)) goto done;
    if (!GetFileSizeEx(file, &length)) { result = VAULT_IO; goto done; }
    if (length.QuadPart < 36 || length.QuadPart > VAULT_WIN_FILE_MAX) goto done;
    *bytes = malloc((size_t)length.QuadPart);
    if (!*bytes) { result = VAULT_INTERNAL; goto done; }
    DWORD total = (DWORD)length.QuadPart, position = 0;
    while (position < total) {
        DWORD count = 0;
        if (!ReadFile(file, *bytes + position, total - position, &count, NULL)) { result = VAULT_IO; goto done; }
        if (!count) goto done;
        position += count;
    }
    if (memcmp(*bytes, magic, sizeof(magic))) goto done;
    *size = total;
    result = VAULT_OK;
done:
    if (!CloseHandle(file) && result == VAULT_OK) result = VAULT_IO;
    if (result) { free(*bytes); *bytes = NULL; *size = 0; }
    return result;
}

int vault_win_publish(vault_win_context *context, const unsigned char *bytes, DWORD size, int add_only) {
    unsigned char nonce[16];
    char hex[33];
    WCHAR temporary[VAULT_PATH_MAX];
    HANDLE file = INVALID_HANDLE_VALUE;
    int created = 0, result = VAULT_IO;
    if (BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return VAULT_INTERNAL;
    vault_hex(nonce, sizeof(nonce), hex);
    if (swprintf_s(temporary, VAULT_PATH_MAX, L"%s\\.pending-%hs", context->directory, hex) < 0)
        return VAULT_IO;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, &context->security, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return vault_win_status(GetLastError());
    created = 1;
    if (!vault_win_regular(file) || !write_all(file, bytes, size) || !FlushFileBuffers(file)) goto done;
    if (!CloseHandle(file)) { file = INVALID_HANDLE_VALUE; goto done; }
    file = INVALID_HANDLE_VALUE;
    DWORD attributes = GetFileAttributesW(context->file);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result = VAULT_INTEGRITY; goto done;
    }
    if (!MoveFileExW(temporary, context->file, (add_only ? 0 : MOVEFILE_REPLACE_EXISTING) | MOVEFILE_WRITE_THROUGH)) goto done;
    created = 0;
    result = VAULT_OK;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (created) DeleteFileW(temporary); /* only the uniquely created ciphertext temporary */
    return result;
}

int64_t hmje_vault_set(const void *ns, int64_t nl, const void *name, int64_t kl,
                       const void *value, int64_t length) {
    if (!vault_names(ns, nl, name, kl) || length < 0 || length > VAULT_VALUE_MAX || (length && !value))
        return VAULT_INVALID;
    vault_win_context context;
    int result = vault_win_begin(&context, ns, nl, name, kl);
    if (result) return result;
    unsigned char key[32] = {0};
    unsigned char *bytes = malloc((size_t)length + 36);
    if (!bytes) { result = VAULT_INTERNAL; goto done; }
    result = vault_win_key(&context, 1, key);
    if (result) goto done;
    memcpy(bytes, magic, sizeof(magic));
    if (BCryptGenRandom(NULL, bytes + 8, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        result = VAULT_INTERNAL; goto done;
    }
    result = vault_win_cipher(1, key, context.aad, bytes + 8, bytes + 20 + length,
                              value, (ULONG)length, bytes + 20);
    if (!result) result = vault_win_publish(&context, bytes, (DWORD)length + 36, 0);
done:
    hmje_vault_wipe(key, sizeof(key));
    free(bytes);
    vault_win_end(&context);
    return result;
}

int64_t hmje_vault_get(const void *ns, int64_t nl, const void *name, int64_t kl,
                       void *out, int64_t capacity, uint64_t *length) {
    if (length) *length = 0;
    if (!length || !vault_names(ns, nl, name, kl) || capacity < 0 || (capacity && !out))
        return VAULT_INVALID;
    vault_win_context context;
    int result = vault_win_begin(&context, ns, nl, name, kl);
    if (result) return result;
    unsigned char key[32] = {0}, *bytes = NULL, *plain = NULL;
    DWORD size = 0, plain_length = 0;
    result = read_cipher(&context, &bytes, &size);
    if (result) goto done;
    plain_length = size - 36;
    if (plain_length > capacity) { result = VAULT_INVALID; goto done; }
    result = vault_win_key(&context, 0, key);
    if (result) goto done;
    plain = malloc(plain_length ? plain_length : 1);
    if (!plain) { result = VAULT_INTERNAL; goto done; }
    result = vault_win_cipher(0, key, context.aad, bytes + 8, bytes + 20 + plain_length,
                              bytes + 20, plain_length, plain);
    if (!result) {
        if (plain_length) memcpy(out, plain, plain_length);
        *length = plain_length;
    }
done:
    hmje_vault_wipe(key, sizeof(key));
    if (plain) { hmje_vault_wipe(plain, plain_length); free(plain); }
    free(bytes);
    vault_win_end(&context);
    return result;
}

int64_t hmje_vault_delete(const void *ns, int64_t nl, const void *name, int64_t kl) {
    if (!vault_names(ns, nl, name, kl)) return VAULT_INVALID;
    vault_win_context context;
    int result = vault_win_begin(&context, ns, nl, name, kl);
    if (result) return result;
    DWORD attributes = GetFileAttributesW(context.file);
    if (attributes == INVALID_FILE_ATTRIBUTES) result = vault_win_status(GetLastError());
    else if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) result = VAULT_INTEGRITY;
    else if (!DeleteFileW(context.file)) result = vault_win_status(GetLastError());
    vault_win_end(&context);
    return result;
}
