#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The installed entry point is a native PE. PowerShell only orchestrates the
 * already-native compiler stages; no Unix shell, Mac share, or VM API is used. */
static int append(wchar_t *out, size_t cap, size_t *used, wchar_t value) {
    if (*used + 1 >= cap) return 0;
    out[(*used)++] = value;
    out[*used] = 0;
    return 1;
}

static int quoted(wchar_t *out, size_t cap, size_t *used, const wchar_t *arg) {
    if (!append(out, cap, used, L'"')) return 0;
    const wchar_t *at = arg;
    while (*at) {
        size_t slashes = 0;
        while (*at == L'\\') { ++slashes; ++at; }
        if (*at == L'"') {
            for (size_t i = 0; i < slashes * 2 + 1; ++i)
                if (!append(out, cap, used, L'\\')) return 0;
            if (!append(out, cap, used, L'"')) return 0;
            ++at;
        } else if (!*at) {
            for (size_t i = 0; i < slashes * 2; ++i)
                if (!append(out, cap, used, L'\\')) return 0;
        } else {
            for (size_t i = 0; i < slashes; ++i)
                if (!append(out, cap, used, L'\\')) return 0;
            if (!append(out, cap, used, *at++)) return 0;
        }
    }
    return append(out, cap, used, L'"');
}

/* hmjec.ini: one UTF-8 line, the absolute path of the active hmjec.ps1. */
static int redirected_script(wchar_t *script, wchar_t *slash) {
    if (wcscpy_s(slash + 1, 32768 - (size_t)(slash + 1 - script), L"hmjec.ini")) return 0;
    HANDLE file = CreateFileW(script, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    char bytes[8192];
    DWORD count = 0;
    BOOL read = ReadFile(file, bytes, sizeof(bytes) - 1, &count, NULL);
    CloseHandle(file);
    if (!read || !count) return 0;
    char *text = bytes;
    if (count >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
        (unsigned char)text[2] == 0xbf) { text += 3; count -= 3; }
    while (count && (text[count - 1] == '\r' || text[count - 1] == '\n' || text[count - 1] == ' '))
        --count;
    if (!count) return 0;
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)count, script, 32767);
    if (units <= 0) return 0;
    script[units] = 0;
    DWORD attributes = GetFileAttributesW(script);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int main(void) {
    wchar_t script[32768], powershell[32768];
    DWORD size = GetModuleFileNameW(NULL, script, 32768);
    if (!size || size >= 32768) return 2;
    wchar_t *slash = wcsrchr(script, L'\\');
    if (!slash || (size_t)(slash - script) + 12 >= 32768) return 2;
    if (wcscpy_s(slash + 1, 32768 - (size_t)(slash + 1 - script), L"hmjec.ps1")) return 2;
    DWORD attributes = GetFileAttributesW(script);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        /* The per-user activation directory (%LOCALAPPDATA%\hmje\bin) holds a
         * copy of this launcher and hmjec.ini naming the active toolchain's
         * hmjec.ps1, so switching versions rewrites one line, not PATH. */
        if (!redirected_script(script, slash)) {
            fputs("hmjec.ps1 is missing beside hmjec.exe (and no hmjec.ini names one)\n", stderr);
            return 2;
        }
    }
    size = GetSystemDirectoryW(powershell, 32768);
    if (!size || size + 43 >= 32768) return 2;
    if (wcscpy_s(powershell + size, 32768 - size,
                 L"\\WindowsPowerShell\\v1.0\\powershell.exe")) return 2;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    wchar_t *command = calloc(32768, sizeof(wchar_t));
    if (!command) { LocalFree(argv); return 2; }
    size_t used = 0;
    int ok = quoted(command, 32768, &used, powershell);
    const wchar_t *fixed[] = {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", script};
    for (size_t i = 0; ok && i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        ok = append(command, 32768, &used, L' ') && quoted(command, 32768, &used, fixed[i]);
    for (int i = 1; ok && i < argc; ++i)
        ok = append(command, 32768, &used, L' ') && quoted(command, 32768, &used, argv[i]);
    LocalFree(argv);
    if (!ok) { free(command); fputs("hmjec command line is too long\n", stderr); return 2; }
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    ok = CreateProcessW(powershell, command, NULL, NULL, TRUE, 0, NULL, NULL,
                        &startup, &child);
    free(command);
    if (!ok) { fprintf(stderr, "PowerShell launch failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    return (int)code;
}
