#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdlib.h>
#include "external_url.h"

static wchar_t *hmje_external_url_parse(const uint8_t *bytes, uint64_t length) {
    if (bytes == NULL || length == 0 || length > 4096) return NULL;
    /* ShellExecute treats a URL as a command target. Never pass embedded NUL,
     * whitespace, controls or backslashes through to its protocol handler. */
    for (uint64_t i = 0; i < length; ++i) {
        if (bytes[i] <= 0x20 || bytes[i] == 0x7f || bytes[i] == '\\') return NULL;
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     (const char *)bytes, (int)length, NULL, 0);
    if (needed <= 0) return NULL;
    wchar_t *url = (wchar_t *)calloc((size_t)needed + 1, sizeof(wchar_t));
    if (url == NULL) return NULL;
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        (const char *)bytes, (int)length, url, needed);
    if (converted != needed) {
        free(url);
        return NULL;
    }
    URL_COMPONENTSW parts = {0};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = 1;
    parts.dwHostNameLength = 1;
    BOOL valid = WinHttpCrackUrl(url, (DWORD)needed, 0, &parts);
    if (!valid || parts.dwHostNameLength == 0 ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        free(url);
        return NULL;
    }
    return url;
}

int64_t hmje_ui_open_external_url(const uint8_t *bytes, uint64_t length) {
    wchar_t *url = hmje_external_url_parse(bytes, length);
    if (url == NULL) return 0;
    HINSTANCE result = ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    free(url);
    return (INT_PTR)result > 32 ? 1 : 0;
}
