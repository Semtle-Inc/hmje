/*
 * windows_open_url.inc — 앱을 연 주소(SYS_APP_OPEN_URL 120)와 URL 스킴 등록·단일 인스턴스
 * 넘기기. windows_window.c가 끝에서 싣는다(창 전역 g_front_window를 쓴다).
 *
 * 흐름:
 *   1. `URL 스킴`을 선언한 GUI 앱만 링크 때 생성되는 app-url.c의 생성자가
 *      hmje_windows_url_startup(스킴들, 개수, 앱 식별자)을 main 전에 부른다.
 *   2. HKCU\Software\Classes\<스킴>에 "URL Protocol"과 shell\open\command
 *      ("<이 exe>" "%1")를 쓴다 — 관리자 권한이 필요 없는 사용자 단위 등록이고, 설치기가
 *      없어도 첫 실행에서 등록된다. 값이 같으면 다시 쓰지 않는다.
 *   3. 명령줄 인자 가운데 선언한 스킴으로 시작하는 것이 주소다.
 *   4. 같은 앱이 이미 돌고 있으면(이름 있는 뮤텍스) 그 인스턴스의 받는 창(메시지 전용,
 *      클래스 HmjeAppUrlReceiver.<식별자>)에 WM_COPYDATA로 주소를 넘기고 이 프로세스는
 *      끝난다. 주소 없이 다시 실행하면 새 인스턴스가 뜬다.
 *   5. 첫 인스턴스는 받는 창을 만들고 명령줄 주소를 큐에 넣는다. 앱 루프가 첫 프레임 뒤에
 *      꺼내므로 차가운 시작의 주소도 잃지 않는다.
 * HMJE_WINDOWS_URL_REGISTER=0이면 2를 건너뛴다(검증·CI에서 레지스트리를 건드리지 않을 때).
 */

#include <shellapi.h> /* CommandLineToArgvW (WIN32_LEAN_AND_MEAN이 뺀다) */

#define HMJE_OPEN_URL_MAX_BYTES (1u << 20)
#define HMJE_OPEN_URL_MAX_PENDING 256
#define HMJE_OPEN_URL_COPYDATA_TAG 0x484D5552u /* 'HMUR' */

static char *g_open_urls[HMJE_OPEN_URL_MAX_PENDING];
static size_t g_open_url_lengths[HMJE_OPEN_URL_MAX_PENDING];
static unsigned g_open_url_head = 0;
static unsigned g_open_url_count = 0;
static HWND g_open_url_receiver = NULL;

static BOOL open_url_debug_enabled(void) {
    return getenv("HMJE_WINDOWS_DEBUG_OPEN_URL") != NULL;
}

static void app_open_url_enqueue_utf8(const char *bytes, size_t length) {
    if (bytes == NULL || length == 0 || length > HMJE_OPEN_URL_MAX_BYTES) return;
    char *copy = (char *)malloc(length);
    if (copy == NULL) return;
    memcpy(copy, bytes, length);
    if (g_open_url_count == HMJE_OPEN_URL_MAX_PENDING) {
        free(g_open_urls[g_open_url_head]);
        g_open_url_head = (g_open_url_head + 1) % HMJE_OPEN_URL_MAX_PENDING;
        g_open_url_count--;
    }
    unsigned slot = (g_open_url_head + g_open_url_count) % HMJE_OPEN_URL_MAX_PENDING;
    g_open_urls[slot] = copy;
    g_open_url_lengths[slot] = length;
    g_open_url_count++;
    if (open_url_debug_enabled()) fprintf(stderr, "windows-window: open-url queued (%u pending)\n", g_open_url_count);
}

static void app_open_url_enqueue_wide(const wchar_t *url) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, url, -1, NULL, 0, NULL, NULL);
    if (needed <= 1) return;
    char *bytes = (char *)malloc((size_t)needed);
    if (bytes == NULL) return;
    WideCharToMultiByte(CP_UTF8, 0, url, -1, bytes, needed, NULL, NULL);
    app_open_url_enqueue_utf8(bytes, (size_t)needed - 1);
    free(bytes);
}

/* a1=0: 다음 주소의 바이트 길이(없으면 0). a1=1: 다음 주소를 a2(용량 a3)에 복사하고
 * 큐에서 뺀다. 용량이 모자라면 복사하지 않고 -34(ERANGE)다. */
i64 hmje_windows_app_open_url(i64 operation, i64 address, i64 capacity) {
    if (operation != 0 && operation != 1) return -22;
    if (g_open_url_count == 0) return 0;
    size_t length = g_open_url_lengths[g_open_url_head];
    if (operation == 0) return (i64)length;
    if (address == 0 || capacity < (i64)length) return -34;
    memcpy((void *)(uintptr_t)(u64)address, g_open_urls[g_open_url_head], length);
    free(g_open_urls[g_open_url_head]);
    g_open_urls[g_open_url_head] = NULL;
    g_open_url_head = (g_open_url_head + 1) % HMJE_OPEN_URL_MAX_PENDING;
    g_open_url_count--;
    if (open_url_debug_enabled()) fprintf(stderr, "windows-window: open-url delivered\n");
    return (i64)length;
}

static LRESULT CALLBACK open_url_receiver_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *data = (const COPYDATASTRUCT *)lparam;
        if (data != NULL && data->dwData == HMJE_OPEN_URL_COPYDATA_TAG && data->lpData != NULL)
            app_open_url_enqueue_utf8((const char *)data->lpData, (size_t)data->cbData);
        /* 보낸 쪽이 AllowSetForegroundWindow로 넘겨 준 권한으로 앞 창을 올린다. */
        if (g_front_window != NULL && g_front_window->hwnd != NULL && !g_front_window->closing) {
            if (IsIconic(g_front_window->hwnd)) ShowWindow(g_front_window->hwnd, SW_RESTORE);
            SetForegroundWindow(g_front_window->hwnd);
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* 스킴은 컴파일러가 검증한 ASCII([a-z][a-z0-9+.-]*)다. 대소문자는 구분하지 않는다. */
static BOOL open_url_has_scheme(const wchar_t *argument, const wchar_t *scheme) {
    size_t length = wcslen(scheme);
    if (length == 0 || _wcsnicmp(argument, scheme, length) != 0) return FALSE;
    return argument[length] == L':';
}

/* 레지스트리 API는 windows_window.c의 테마 읽기처럼 advapi32.dll에서 동적으로 싣는다 —
 * 링크 레시피에 새 가져오기 라이브러리를 더하지 않는다. */
typedef LSTATUS (WINAPI *open_url_reg_get_value)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LSTATUS (WINAPI *open_url_reg_set_value)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPCVOID, DWORD);
static open_url_reg_get_value g_open_url_reg_get = NULL;
static open_url_reg_set_value g_open_url_reg_set = NULL;

static void open_url_registry_set(const wchar_t *subkey, const wchar_t *name, const wchar_t *value) {
    if (g_open_url_reg_get == NULL || g_open_url_reg_set == NULL) return;
    wchar_t current[4096];
    DWORD size = sizeof(current);
    if (g_open_url_reg_get(HKEY_CURRENT_USER, subkey, name, RRF_RT_REG_SZ, NULL, current, &size) == ERROR_SUCCESS
        && wcscmp(current, value) == 0) return;
    LSTATUS status = g_open_url_reg_set(HKEY_CURRENT_USER, subkey, name, REG_SZ, value,
                                        (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
        fprintf(stderr, "windows-window: URL scheme registration failed (%ld)\n", (long)status);
}

static void open_url_register_scheme(const wchar_t *scheme, const wchar_t *executable) {
    wchar_t key[512];
    wchar_t text[512];
    static wchar_t command[34000];
    _snwprintf_s(key, 512, _TRUNCATE, L"Software\\Classes\\%ls", scheme);
    _snwprintf_s(text, 512, _TRUNCATE, L"URL:%ls", scheme);
    open_url_registry_set(key, NULL, text);
    open_url_registry_set(key, L"URL Protocol", L"");
    _snwprintf_s(key, 512, _TRUNCATE, L"Software\\Classes\\%ls\\shell\\open\\command", scheme);
    _snwprintf_s(command, 34000, _TRUNCATE, L"\"%ls\" \"%%1\"", executable);
    open_url_registry_set(key, NULL, command);
}

/* app-url.c(링크 때 생성)가 main 전에 부른다. */
void hmje_windows_url_startup(const wchar_t *const *schemes, int count, const wchar_t *identity) {
    if (schemes == NULL || count <= 0 || identity == NULL || identity[0] == 0) return;
    const char *register_flag = getenv("HMJE_WINDOWS_URL_REGISTER");
    if (register_flag == NULL || strcmp(register_flag, "0") != 0) {
        static wchar_t executable[32768];
        HMODULE advapi = LoadLibraryW(L"advapi32.dll");
        if (advapi != NULL) {
            g_open_url_reg_get = (open_url_reg_get_value)(void *)GetProcAddress(advapi, "RegGetValueW");
            g_open_url_reg_set = (open_url_reg_set_value)(void *)GetProcAddress(advapi, "RegSetKeyValueW");
            DWORD written = GetModuleFileNameW(NULL, executable, 32768);
            if (written > 0 && written < 32768)
                for (int index = 0; index < count; index++) open_url_register_scheme(schemes[index], executable);
            g_open_url_reg_get = NULL;
            g_open_url_reg_set = NULL;
            FreeLibrary(advapi);
        }
    }

    /* 명령줄의 주소(선언한 스킴으로 시작하는 인자). */
    const wchar_t *startup_url = NULL;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int index = 1; argv != NULL && index < argc && startup_url == NULL; index++)
        for (int scheme = 0; scheme < count; scheme++)
            if (open_url_has_scheme(argv[index], schemes[scheme])) { startup_url = argv[index]; break; }

    wchar_t name[512];
    _snwprintf_s(name, 512, _TRUNCATE, L"Local\\HmjeAppUrl.%ls", identity);
    wchar_t receiver_class[512];
    _snwprintf_s(receiver_class, 512, _TRUNCATE, L"HmjeAppUrlReceiver.%ls", identity);
    HANDLE mutex = CreateMutexW(NULL, FALSE, name);
    if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS && startup_url != NULL) {
        /* 이미 도는 인스턴스에 넘긴다. 그 인스턴스가 막 뜨는 중이면 받는 창이 생길 때까지
         * 조금 기다린다. 받는 창이 끝내 없으면(멈췄거나 끝나는 중) 이 프로세스가 연다. */
        for (int attempt = 0; attempt < 40; attempt++) {
            HWND receiver = FindWindowExW(HWND_MESSAGE, NULL, receiver_class, NULL);
            if (receiver != NULL) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, startup_url, -1, NULL, 0, NULL, NULL);
                char *bytes = needed > 1 ? (char *)malloc((size_t)needed) : NULL;
                if (bytes != NULL) {
                    WideCharToMultiByte(CP_UTF8, 0, startup_url, -1, bytes, needed, NULL, NULL);
                    DWORD process = 0;
                    GetWindowThreadProcessId(receiver, &process);
                    if (process != 0) AllowSetForegroundWindow(process);
                    COPYDATASTRUCT data;
                    data.dwData = HMJE_OPEN_URL_COPYDATA_TAG;
                    data.cbData = (DWORD)(needed - 1);
                    data.lpData = bytes;
                    DWORD_PTR result = 0;
                    LRESULT sent = SendMessageTimeoutW(receiver, WM_COPYDATA, 0, (LPARAM)&data,
                                                       SMTO_ABORTIFHUNG, 5000, &result);
                    free(bytes);
                    if (sent != 0 && result == TRUE) {
                        if (open_url_debug_enabled()) fprintf(stderr, "windows-window: open-url forwarded\n");
                        LocalFree(argv);
                        ExitProcess(0);
                    }
                }
                break;
            }
            Sleep(50);
        }
    }

    WNDCLASSEXW receiver_window_class;
    ZeroMemory(&receiver_window_class, sizeof(receiver_window_class));
    receiver_window_class.cbSize = sizeof(receiver_window_class);
    receiver_window_class.lpfnWndProc = open_url_receiver_proc;
    receiver_window_class.hInstance = GetModuleHandleW(NULL);
    receiver_window_class.lpszClassName = receiver_class;
    if (RegisterClassExW(&receiver_window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        g_open_url_receiver = CreateWindowExW(0, receiver_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                              GetModuleHandleW(NULL), NULL);
    if (startup_url != NULL) app_open_url_enqueue_wide(startup_url);
    if (argv != NULL) LocalFree(argv);
    /* 뮤텍스는 프로세스가 끝날 때까지 쥔다(닫지 않는다). */
}
