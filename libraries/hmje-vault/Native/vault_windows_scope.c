#include "vault_windows.h"
#include <appmodel.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>

int vault_win_status(DWORD error) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NOT_FOUND)
        return VAULT_MISSING;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_NO_SUCH_LOGON_SESSION ||
        error == ERROR_PASSWORD_RESTRICTION) return VAULT_LOCKED;
    return VAULT_IO;
}

static int app_identity(char out[2048]) {
    /* Folder ACLs, profile location and credentials must refer to one user.
     * This desktop backend deliberately does not support impersonation. */
    HANDLE token = NULL;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        CloseHandle(token);
        return VAULT_UNSUPPORTED;
    }
    if (GetLastError() != ERROR_NO_TOKEN) return VAULT_LOCKED;
    PWSTR explicit_id = NULL;
    WCHAR packaged[512];
    WCHAR *identity = NULL;
    if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&explicit_id)) && explicit_id)
        identity = explicit_id;
    else {
        UINT32 size = 512;
        if (GetCurrentApplicationUserModelId(&size, packaged) == ERROR_SUCCESS) identity = packaged;
    }
    int count = identity ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, identity, -1,
                                               out, 2048, NULL, NULL) : 0;
    if (explicit_id) CoTaskMemFree(explicit_id);
    return count > 1 ? VAULT_OK : VAULT_UNSUPPORTED;
}

static int digest(const char *label, const char *app, const void *ns, ULONG nl,
                  const void *key, ULONG kl, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char zero = 0;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER, 0) < 0) goto done;
    if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    const char *domain = "HMJE-VAULT-WIN-1";
    const void *parts[] = {domain, label, app, ns, key};
    ULONG lengths[] = {(ULONG)strlen(domain), (ULONG)strlen(label), (ULONG)strlen(app), nl, kl};
    for (size_t i = 0; i < 5; ++i) {
        if (lengths[i] && BCryptHashData(hash, (PUCHAR)parts[i], lengths[i], 0) < 0) goto done;
        if (BCryptHashData(hash, &zero, 1, 0) < 0) goto done;
    }
    ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

static void wide_hex(const unsigned char hash[32], WCHAR text[65]) {
    const WCHAR *hex = L"0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        text[i * 2] = hex[hash[i] >> 4];
        text[i * 2 + 1] = hex[hash[i] & 15];
    }
    text[64] = 0;
}

static int security(vault_win_context *context) {
    HANDLE token = NULL;
    union { TOKEN_USER align; unsigned char bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } data;
    DWORD needed = 0;
    LPWSTR sid = NULL;
    WCHAR sddl[512];
    int ok = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto done;
    if (!GetTokenInformation(token, TokenUser, &data, sizeof(data), &needed)) goto done;
    if (!ConvertSidToStringSidW(((TOKEN_USER *)&data)->User.Sid, &sid)) goto done;
    if (swprintf_s(sddl, 512, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%s)", sid) < 0) goto done;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                              &context->descriptor, NULL)) goto done;
    context->security.nLength = sizeof(context->security);
    context->security.lpSecurityDescriptor = context->descriptor;
    context->security.bInheritHandle = FALSE;
    ok = 1;
done:
    if (sid) LocalFree(sid);
    if (token) CloseHandle(token);
    return ok;
}

static int directory(const WCHAR *path, SECURITY_ATTRIBUTES *attributes) {
    if (!CreateDirectoryW(path, attributes) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(handle, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    return ok;
}

static int root(vault_win_context *context, const char *app, const WCHAR *family) {
    PWSTR local = NULL;
    unsigned char hash[32];
    WCHAR hex[65];
    int result = VAULT_IO;
    if (!digest("app", app, NULL, 0, NULL, 0, hash)) return VAULT_INTERNAL;
    wide_hex(hash, hex);
    if (!security(context)) return VAULT_IO;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &local))) goto done;
    /* Extended-length absolute paths. The remaining components are fixed or hashes. */
    int count;
    if (local[0] == L'\\' && local[1] == L'\\')
        count = swprintf_s(context->directory, VAULT_PATH_MAX, L"\\\\?\\UNC\\%s\\%s", local + 2, family);
    else if (wcslen(local) >= 3 && local[1] == L':' && local[2] == L'\\')
        count = swprintf_s(context->directory, VAULT_PATH_MAX, L"\\\\?\\%s\\%s", local, family);
    else goto done;
    if (count < 0 || count + 70 >= VAULT_PATH_MAX || !directory(context->directory, &context->security)) goto done;
    wcscat_s(context->directory, VAULT_PATH_MAX, L"\\");
    wcscat_s(context->directory, VAULT_PATH_MAX, hex);
    if (!directory(context->directory, &context->security)) goto done;
    result = VAULT_OK;
done:
    if (local) CoTaskMemFree(local);
    return result;
}

int vault_win_regular(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

void vault_win_end(vault_win_context *context) {
    if (context->lock && context->lock != INVALID_HANDLE_VALUE) CloseHandle(context->lock);
    if (context->descriptor) LocalFree(context->descriptor);
    context->lock = NULL;
    context->descriptor = NULL;
}

int vault_win_begin(vault_win_context *context, const void *ns, int64_t nl,
                    const void *key, int64_t kl) {
    memset(context, 0, sizeof(*context));
    char app[2048];
    unsigned char scope[32];
    WCHAR scope_hex[65], entry_hex[65], lock_path[VAULT_PATH_MAX];
    int result = app_identity(app);
    if (result) return result;
    if (!digest("namespace", app, ns, (ULONG)nl, NULL, 0, scope) ||
        !digest("entry", app, ns, (ULONG)nl, key, (ULONG)kl, context->aad)) return VAULT_INTERNAL;
    wide_hex(scope, scope_hex);
    wide_hex(context->aad, entry_hex);
    swprintf_s(context->credential, 128, L"net.semtle.hmje-vault.v1.%s", scope_hex);
    result = root(context, app, L"hmje-vault-v1");
    if (result) goto failed;
    size_t used = wcslen(context->directory);
    if (used + 150 >= VAULT_PATH_MAX) { result = VAULT_IO; goto failed; }
    wcscat_s(context->directory, VAULT_PATH_MAX, L"\\");
    wcscat_s(context->directory, VAULT_PATH_MAX, scope_hex);
    if (!directory(context->directory, &context->security)) { result = VAULT_IO; goto failed; }
    swprintf_s(context->file, VAULT_PATH_MAX, L"%s\\%s.bin", context->directory, entry_hex);
    swprintf_s(lock_path, VAULT_PATH_MAX, L"%s\\namespace.lock", context->directory);
    ULONGLONG start = GetTickCount64();
    for (;;) {
        context->lock = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
            &context->security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (context->lock != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION || GetTickCount64() - start >= 5000) {
            result = vault_win_status(error); goto failed;
        }
        Sleep(10);
    }
    if (!vault_win_regular(context->lock)) { result = VAULT_INTEGRITY; goto failed; }
    return VAULT_OK;
failed:
    vault_win_end(context);
    return result;
}

int64_t hmje_vault_app_data_path(void *out, int64_t capacity, uint64_t *length) {
    if (length) *length = 0;
    if (!length || !out || capacity <= 0) return VAULT_INVALID;
    vault_win_context context = {0};
    char app[2048], path[VAULT_PATH_MAX];
    int result = app_identity(app);
    if (!result) result = root(&context, app, L"hmje-data");
    if (!result) {
        int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, context.directory, -1,
                                        path, sizeof(path), NULL, NULL);
        if (count <= 1 || count - 1 > capacity) result = VAULT_INVALID;
        else { memcpy(out, path, (size_t)count - 1); *length = (uint64_t)count - 1; }
    }
    vault_win_end(&context);
    return result;
}
