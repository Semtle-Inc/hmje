/* ---- 셀프테스트 (windows_window.c에 포함) ---------------------------------------
 * macOS HMJE_POSIX_SELFTEST_POINTER의 Windows 짝. 스크립트는 HMJE_WINDOWS_SELFTEST 또는
 * (같은 시험 대본을 두 호스트에서 돌리도록) HMJE_POSIX_SELFTEST_POINTER에서 읽는다. 시작
 * 프레임은 HMJE_WINDOWS_SELFTEST_AT / HMJE_POSIX_SELFTEST_POINTER_AT(기본 30).
 * 앞 창의 빈 입력 poll(프레임)마다 동작 하나:
 *   wait:N · waitms:N
 *   move:x,y · drag:x,y · press:B · release:B · click:x,y(이동+누름+뗌)
 *   scroll:dy · scroll:dx,dy · wheel:dx,dy        (좌표는 클라이언트 픽셀)
 *   trackpad:속도,손가락ms[,Hz]                  (정밀 터치패드 흐름 + 관성, 아래 설명)
 *   key:<macOS 키코드>[,csot]                   (c·t = Ctrl, s = Shift, o = Alt)
 *   ime:<UTF-8> · commit:<UTF-8>                 (IMM 조합/확정 — 아래 가짜 IMM)
 *   text:<UTF-8> · code:<16진,…>                 (IME 확정과 같은 경로)
 *   resize:WxH(포인트) · close · quit · ctrlw · ctrlq · window:<ID>
 * 창 메시지를 실제 window_proc에 태우므로 좌표 변환·인코딩·단축키·앱 반응이 다 지난다
 * (시스템 입력 큐는 지나지 않는다). 수정자 키는 이 스레드의 키 상태표(SetKeyboardState)에
 * 잠깐 적어 GetKeyState가 그대로 읽게 한다.
 *
 * ime:/commit:이 든 대본은 가짜 IMM(g_imm_test 훅)을 건다: 조합 글을 GCS_COMPSTR로
 * 보이고, 호스트가 조합을 끝낼 때(클릭·Ctrl 단축키 = CPS_COMPLETE) 한국어 IME처럼 그
 * 글을 GCS_RESULTSTR로 확정한다 — 진짜 IME가 보내는 WM_IME_* 순서 그대로다.
 *
 * HMJE_WINDOWS_DUMP_FRAME_DIR 또는 HMJE_POSIX_DUMP_FRAME_DIR: present마다 CPU 표면을
 * <dir>/frame-NNNNNN.png로 남긴다. 셀프테스트 중에는 번호가 동작 번호다(1부터, 한
 * 동작의 여러 present는 같은 파일을 덮어써 정착된 프레임이 남는다 — macOS와 같다). */

#include <wincodec.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

static BOOL g_selftest_active = FALSE;
static u64 g_selftest_step = 0;
static u64 g_dump_present = 0;

/* UTF-8 value of an environment variable (the ANSI API would lose Korean text on a
 * CP949 system). */
static const char *selftest_env_one(const char *name, char *buffer, DWORD size) {
    WCHAR wide_name[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 128) <= 0) return NULL;
    DWORD needed = GetEnvironmentVariableW(wide_name, NULL, 0);
    if (needed == 0 || needed > 65536) return NULL;
    WCHAR *wide = (WCHAR *)malloc((size_t)needed * sizeof(WCHAR));
    if (wide == NULL) return NULL;
    DWORD length = GetEnvironmentVariableW(wide_name, wide, needed);
    int bytes = length > 0 && length < needed
        ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, (int)size, NULL, NULL) : 0;
    free(wide);
    return bytes > 0 ? buffer : NULL;
}

static const char *selftest_env(const char *primary, const char *fallback, char *buffer, DWORD size) {
    const char *value = selftest_env_one(primary, buffer, size);
    return value != NULL ? value : selftest_env_one(fallback, buffer, size);
}

static hmje_windows_window *selftest_target(void) {
    if (g_front_window != NULL && !g_front_window->closing) return g_front_window;
    hmje_windows_window *found = NULL;
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
        if (!window->closing && window->shown && found == NULL) found = window;
    return found != NULL ? found : (g_windows != NULL && !g_windows->closing ? g_windows : NULL);
}

/* ---- 프레임 덤프(WIC PNG) ---- */
static void selftest_write_png(const wchar_t *path, const unsigned char *pixels, u64 width, u64 height, u64 pitch) {
    static BOOL com_ready = FALSE;
    if (!com_ready) { (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); com_ready = TRUE; }
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, (UINT)width, (UINT)height);
    /* SetPixelFormat is in/out: the PNG encoder answers with the closest format it
     * writes (32bppBGR comes back as 24bppBGR). Rows are converted to whatever it
     * accepted — handing it the 4-byte source anyway stretched every frame by 4/3. */
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    unsigned bytes = IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR) ? 3u
                   : (IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA) ||
                      IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGR)) ? 4u : 0u;
    if (SUCCEEDED(hr) && bytes == 0) hr = E_NOTIMPL;
    unsigned char *rows = NULL;
    u64 stride = width * bytes;
    if (SUCCEEDED(hr)) {
        rows = (unsigned char *)malloc((size_t)(stride * height));
        if (rows == NULL) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        for (u64 y = 0; y < height; ++y) {
            const unsigned char *from = pixels + y * pitch;
            unsigned char *to = rows + y * stride;
            for (u64 x = 0; x < width; ++x, from += 4, to += bytes) {
                to[0] = from[0]; to[1] = from[1]; to[2] = from[2];
                if (bytes == 4) to[3] = 0xFF; /* the swap chain's alpha is not coverage */
            }
        }
        hr = IWICBitmapFrameEncode_WritePixels(frame, (UINT)height, (UINT)stride,
                                               (UINT)(stride * height), rows);
    }
    free(rows);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) fprintf(stderr, "windows-surface: selftest frame dump failed hr=0x%08lx\n", (unsigned long)hr);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
}

static void selftest_after_present(void) {
    char directory[1024];
    if (!selftest_env("HMJE_WINDOWS_DUMP_FRAME_DIR", "HMJE_POSIX_DUMP_FRAME_DIR", directory, sizeof(directory))) return;
    if (g_selftest_active && g_win != selftest_target()) return;
    if (g_staging_tex == NULL || g_context == NULL || g_width == 0 || g_height == 0) return;
    u64 index = g_selftest_active ? g_selftest_step : g_dump_present++;
    char path[1200];
    snprintf(path, sizeof(path), "%s\\frame-%06llu.png", directory, (unsigned long long)index);
    wchar_t wide[1200];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1200) <= 0) return;
    /* The staging texture holds the frame just presented, whichever path drew it
     * (CPU blits or the direct D3D11 command renderer). */
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)g_staging_tex, 0, D3D11_MAP_READ, 0, &mapped))) {
        fprintf(stderr, "windows-surface: selftest frame readback failed\n");
        return;
    }
    selftest_write_png(wide, (const unsigned char *)mapped.pData, g_width, g_height, mapped.RowPitch);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)g_staging_tex, 0);
}

/* ---- 가짜 IMM ---- */
static WCHAR g_st_comp[512];
static size_t g_st_comp_len = 0;
static WCHAR g_st_result[512];
static size_t g_st_result_len = 0;

static HIMC st_get_context(HWND window) { (void)window; return (HIMC)(uintptr_t)1; }
static BOOL st_release_context(HWND window, HIMC context) { (void)window; (void)context; return TRUE; }
static LONG st_get_composition(HIMC context, DWORD index, LPVOID buffer, DWORD bytes) {
    (void)context;
    const WCHAR *source = NULL;
    size_t count = 0;
    if (index == GCS_COMPSTR) { source = g_st_comp; count = g_st_comp_len; }
    else if (index == GCS_RESULTSTR) { source = g_st_result; count = g_st_result_len; }
    else if (index == GCS_CURSORPOS) return (LONG)g_st_comp_len;
    else return 0;
    LONG need = (LONG)(count * sizeof(WCHAR));
    if (buffer == NULL || bytes == 0) return need;
    if (bytes < (DWORD)need) return IMM_ERROR_GENERAL;
    memcpy(buffer, source, (size_t)need);
    return need;
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static void st_send_result(HWND hwnd) {
    window_proc(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    g_st_result_len = 0;
    window_proc(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
}
static BOOL st_notify(HIMC context, DWORD action, DWORD index, DWORD value) {
    (void)context; (void)value;
    if (action != NI_COMPOSITIONSTR) return TRUE;
    if (index == CPS_COMPLETE && g_st_comp_len > 0) {
        memcpy(g_st_result, g_st_comp, g_st_comp_len * sizeof(WCHAR));
        g_st_result_len = g_st_comp_len;
        g_st_comp_len = 0;
        st_send_result(g_hwnd);
    } else if (index == CPS_CANCEL) {
        g_st_comp_len = 0;
    }
    return TRUE;
}
static UINT st_get_virtual_key(HWND window) { (void)window; return 0; }
static BOOL st_set_composition_window(HIMC context, COMPOSITIONFORM *form) { (void)context; (void)form; return TRUE; }
static BOOL st_set_candidate_window(HIMC context, CANDIDATEFORM *form) { (void)context; (void)form; return TRUE; }
static const hmje_imm_hooks g_selftest_imm = {
    st_get_context, st_release_context, st_get_composition, st_notify,
    st_get_virtual_key, st_set_composition_window, st_set_candidate_window,
};

static size_t selftest_utf16(const char *text, WCHAR *out, size_t capacity) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)capacity);
    return count > 1 ? (size_t)(count - 1) : 0;
}

/* ---- 키 ---- */
static UINT selftest_mac_key(unsigned code) {
    static const unsigned char letters[64] = {
        'A','S','D','F','H','G','Z','X','C','V',0,'B','Q','W','E','R','Y','T','1','2','3','4','6','5',
        VK_OEM_PLUS,'9','7',VK_OEM_MINUS,'8','0',VK_OEM_6,'O','U',VK_OEM_4,'I','P',VK_RETURN,'L','J',VK_OEM_7,
        'K',VK_OEM_1,VK_OEM_5,VK_OEM_COMMA,VK_OEM_2,'N','M',VK_OEM_PERIOD,VK_TAB,VK_SPACE,VK_OEM_3,VK_BACK,0,VK_ESCAPE,
        0,0,0,0,0,0,0,0,0,0
    };
    if (code < 54 && letters[code]) return letters[code];
    switch (code) {
        case 76: return VK_RETURN;
        case 117: return VK_DELETE;
        case 115: return VK_HOME;
        case 119: return VK_END;
        case 116: return VK_PRIOR;
        case 121: return VK_NEXT;
        case 123: return VK_LEFT;
        case 124: return VK_RIGHT;
        case 125: return VK_DOWN;
        case 126: return VK_UP;
        case 122: return VK_F1;
        case 120: return VK_F2;
        case 99: return VK_F3;
        case 118: return VK_F4;
        case 96: return VK_F5;
        default: return 0;
    }
}

static void selftest_key(HWND hwnd, unsigned code, const char *mods) {
    UINT vk = selftest_mac_key(code);
    if (vk == 0) { fprintf(stderr, "windows-surface: selftest 모르는 키코드 %u\n", code); return; }
    BOOL control = strchr(mods, 'c') != NULL || strchr(mods, 't') != NULL;
    BOOL shift = strchr(mods, 's') != NULL;
    BOOL alt = strchr(mods, 'o') != NULL;
    BYTE saved[256], state[256];
    GetKeyboardState(saved);
    memcpy(state, saved, sizeof(state));
    state[VK_CONTROL] = state[VK_LCONTROL] = control ? 0x80 : 0;
    state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
    state[VK_MENU] = state[VK_LMENU] = alt ? 0x80 : 0;
    state[VK_RMENU] = 0;
    SetKeyboardState(state);
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 255;
    BOOL extended = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME ||
                    vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE;
    LPARAM down = 1 | ((LPARAM)scan << 16) | (extended ? (1 << 24) : 0) | (alt ? (1 << 29) : 0);
    LPARAM up = down | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    BOOL system = alt && !control;
    window_proc(hwnd, system ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, down);
    /* Printable keys without Ctrl/Alt also produce WM_CHAR, as TranslateMessage does. */
    if (!control && !alt) {
        WCHAR chars[4];
        int produced = ToUnicode(vk, scan, state, chars, 4, 0);
        for (int i = 0; i < produced; ++i) window_proc(hwnd, WM_CHAR, chars[i], down);
    }
    window_proc(hwnd, system ? WM_SYSKEYUP : WM_KEYUP, vk, up);
    SetKeyboardState(saved);
}

static void selftest_resize(HWND hwnd, u64 width, u64 height) {
    RECT rect = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&rect, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* HMJE_WINDOWS_SELFTEST_RESIZE / HMJE_POSIX_SELFTEST_RESIZE = 너비x높이[@프레임](포인트):
 * 지정한 poll 수(기본 90) 뒤 앞 창의 클라이언트를 그 크기(× 표면 배율)로 바꾼다. */
static void maybe_selftest_resize(void) {
    static BOOL done = FALSE;
    static u64 frames = 0;
    if (done) return;
    char spec[64];
    if (!selftest_env("HMJE_WINDOWS_SELFTEST_RESIZE", "HMJE_POSIX_SELFTEST_RESIZE", spec, sizeof(spec))) {
        done = TRUE;
        return;
    }
    if (g_win == NULL || g_win != selftest_target()) return;
    unsigned long long width = 0, height = 0, at = 90;
    if (sscanf(spec, "%llux%llu@%llu", &width, &height, &at) < 2 || width == 0 || height == 0) {
        done = TRUE;
        return;
    }
    if (++frames < at) return;
    done = TRUE;
    fprintf(stderr, "windows-surface: selftest resize %llux%llu points (scale %llu)\n", width, height,
            (unsigned long long)g_scale);
    selftest_resize(g_hwnd, width * g_scale, height * g_scale);
}

/* ---- 트랙패드 흐름 ----
 * `trackpad:속도,손가락ms[,Hz]`(macOS 호스트와 같은 뜻): 손가락이 `속도`(포인트/초, 음수 = 문서
 * 아래로)로 `손가락ms` 동안 쓸고 떼면 관성이 지수 감속(시정수 350ms, 20pt/s 아래에서 끝)으로
 * 이어진다. 별도 스레드가 실제 시계로 Hz(기본 120)마다 정밀 터치패드처럼 한 노치보다 작은
 * WM_MOUSEWHEEL을 창 큐에 넣는다(한 노치 = 48pt). 앱이 바쁘면 실제 기기처럼 쌓인다. */
typedef struct { HWND hwnd; POINT at; double velocity, finger_ms, hz; } selftest_trackpad_spec;

static void selftest_trackpad_post(selftest_trackpad_spec *spec, double points, double *carry) {
    *carry += points * (double)WHEEL_DELTA / 48.0;
    long units = (long)(*carry >= 0.0 ? floor(*carry) : ceil(*carry));
    if (units == 0) return;
    *carry -= (double)units;
    if (units % WHEEL_DELTA == 0) units += units > 0 ? -1 : 1; /* stay below notch granularity */
    PostMessageW(spec->hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)units), MAKELPARAM((WORD)(short)spec->at.x, (WORD)(short)spec->at.y));
}

static DWORD WINAPI selftest_trackpad_thread(LPVOID argument) {
    selftest_trackpad_spec spec = *(selftest_trackpad_spec *)argument;
    free(argument);
    double hz = spec.hz > 0.0 ? spec.hz : 120.0, carry = 0.0;
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    double period = (double)frequency.QuadPart / hz, next = (double)now.QuadPart;
    double finger_end = next + spec.finger_ms * (double)frequency.QuadPart / 1000.0;
    for (int momentum = 0, sample = 0;; ++sample) {
        next += period;
        double velocity = spec.velocity;
        if (next > finger_end) {
            if (!momentum) { momentum = 1; sample = 0; }
            velocity = spec.velocity * exp(-(double)sample / hz / 0.35);
            if (fabs(velocity) < 20.0) break;
        }
        for (;;) {
            QueryPerformanceCounter(&now);
            double left = next - (double)now.QuadPart;
            if (left <= 0.0) break;
            Sleep((DWORD)(left * 1000.0 / (double)frequency.QuadPart) > 0 ? (DWORD)(left * 1000.0 / (double)frequency.QuadPart) : 0);
        }
        selftest_trackpad_post(&spec, velocity / hz, &carry);
    }
    return 0;
}

static void selftest_trackpad_start(HWND hwnd, LONG x, LONG y, double velocity, double finger_ms, double hz) {
    selftest_trackpad_spec *spec = (selftest_trackpad_spec *)malloc(sizeof *spec);
    if (spec == NULL) return;
    spec->hwnd = hwnd;
    spec->at.x = x; spec->at.y = y;
    ClientToScreen(hwnd, &spec->at);
    spec->velocity = velocity; spec->finger_ms = finger_ms; spec->hz = hz;
    HANDLE thread = CreateThread(NULL, 0, selftest_trackpad_thread, spec, 0, NULL);
    if (thread == NULL) { free(spec); return; }
    CloseHandle(thread);
}

static void maybe_selftest(void) {
    maybe_selftest_resize();
    static BOOL done = FALSE, started = FALSE;
    static char script[65536];
    static char *step = NULL;
    static u64 frames = 0, wait_until = 30;
    static ULONGLONG wait_until_ms = 0;
    static LONG cursor_x = 0, cursor_y = 0;
    static WPARAM buttons = 0;
    if (done) return;
    if (!started) {
        started = TRUE;
        if (!selftest_env("HMJE_WINDOWS_SELFTEST", "HMJE_POSIX_SELFTEST_POINTER", script, sizeof(script))) {
            done = TRUE;
            return;
        }
        char at[32];
        if (selftest_env("HMJE_WINDOWS_SELFTEST_AT", "HMJE_POSIX_SELFTEST_POINTER_AT", at, sizeof(at)))
            wait_until = strtoull(at, NULL, 10);
        if (strstr(script, "ime:") != NULL || strstr(script, "commit:") != NULL) g_imm_test = &g_selftest_imm;
        step = script;
        g_selftest_active = TRUE;
    }
    if (g_win == NULL || g_win != selftest_target()) return;
    static BOOL made_key = FALSE;
    if (!made_key) {
        /* makeKeyAndOrderFront: ask for the foreground and tell the app it is key even
         * when Windows' foreground lock refuses (WM_ACTIVATE keeps it key, above). */
        made_key = TRUE;
        SetForegroundWindow(g_hwnd);
        if (g_window_events_aware && !HMJE_WIN->closing) window_push_event(HMJE_WIN, HMJE_WINDOW_EVENT_BECAME_KEY);
    }
    frames++;
    if (frames < wait_until) return;
    if (wait_until_ms != 0) {
        if (GetTickCount64() < wait_until_ms) return;
        wait_until_ms = 0;
    }
    if (step == NULL || *step == '\0') {
        done = TRUE;
        g_selftest_active = FALSE;
        if (g_dump_present <= g_selftest_step) g_dump_present = g_selftest_step + 1;
        fprintf(stderr, "windows-selftest: done\n");
        fflush(stderr);
        /* 셀프테스트 러너(시험 도구)가 로그를 끝까지 기다리지 않고 알 수 있게 한다. */
        char marker[1024];
        if (GetEnvironmentVariableA("HMJE_WINDOWS_SELFTEST_DONE_FILE", marker, sizeof(marker)) - 1u < sizeof(marker) - 1u) {
            FILE *file = fopen(marker, "wb");
            if (file != NULL) fclose(file);
        }
        return;
    }
    char *end = strchr(step, ';');
    if (end != NULL) *end = '\0';
    char *action = step;
    step = end != NULL ? end + 1 : NULL;
    g_selftest_step++;
    unsigned long long a = 0, b = 0;
    long long sa = 0, sb = 0;
    unsigned code = 0;
    char mods[8] = {0};
    HWND hwnd = g_hwnd;
    if (sscanf(action, "waitms:%llu", &a) == 1) {
        wait_until_ms = GetTickCount64() + a;
    } else if (sscanf(action, "wait:%llu", &a) == 1) {
        wait_until = frames + a;
    } else if (sscanf(action, "move:%llu,%llu", &a, &b) == 2 || sscanf(action, "drag:%llu,%llu", &a, &b) == 2) {
        cursor_x = (LONG)a;
        cursor_y = (LONG)b;
        window_proc(hwnd, WM_MOUSEMOVE, buttons, MAKELPARAM((WORD)cursor_x, (WORD)cursor_y));
    } else if (sscanf(action, "press:%llu", &a) == 1) {
        UINT msg = a == 1 ? WM_RBUTTONDOWN : a == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
        buttons |= a == 1 ? MK_RBUTTON : a == 2 ? MK_MBUTTON : MK_LBUTTON;
        window_proc(hwnd, msg, buttons, MAKELPARAM((WORD)cursor_x, (WORD)cursor_y));
    } else if (sscanf(action, "release:%llu", &a) == 1) {
        UINT msg = a == 1 ? WM_RBUTTONUP : a == 2 ? WM_MBUTTONUP : WM_LBUTTONUP;
        buttons &= ~(WPARAM)(a == 1 ? MK_RBUTTON : a == 2 ? MK_MBUTTON : MK_LBUTTON);
        window_proc(hwnd, msg, buttons, MAKELPARAM((WORD)cursor_x, (WORD)cursor_y));
    } else if (sscanf(action, "click:%llu,%llu", &a, &b) == 2) {
        cursor_x = (LONG)a;
        cursor_y = (LONG)b;
        LPARAM point = MAKELPARAM((WORD)a, (WORD)b);
        window_proc(hwnd, WM_MOUSEMOVE, 0, point);
        window_proc(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        window_proc(hwnd, WM_LBUTTONUP, 0, point);
    } else if (sscanf(action, "wheel:%lld,%lld", &sa, &sb) == 2 ||
               sscanf(action, "scroll:%lld,%lld", &sa, &sb) == 2 ||
               (sa = 0, sscanf(action, "scroll:%lld", &sb) == 1)) {
        /* wheel:는 노치 수(실제 WM_MOUSEWHEEL), scroll:은 macOS 호스트와 같은 정밀 스크롤 표본
         * (POLL2 포인트 델타 + 같은 양의 레거시 스크롤) — 두 호스트가 같은 대본에 같은 만큼
         * 굴린다. macOS 부호: 양수 = 위/왼쪽. WM_MOUSEWHEEL은 화면 좌표를 싣는다. */
        BOOL notches = strncmp(action, "wheel:", 6) == 0;
        if (!notches) {
            windows_pointer_push(HMJE_POINTER_PHASE_SCROLL, 1u, cursor_x, cursor_y, 0u, 0u, 0u, 0u, 0u,
                                 (i64)sa, (i64)sb, HMJE_POINTER_TOOL_MOUSE,
                                 HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL);
            legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, (u64)cursor_x, (u64)cursor_y, 0u, FALSE));
            if (sb != 0) {
                u64 amount = (u64)(sb < 0 ? -sb : sb);
                u64 packed = (HMJE_INPUT_KIND_POINTER_SCROLL << HMJE_INPUT_KIND_SHIFT)
                           | (amount & HMJE_INPUT_POINTER_COORD_MASK);
                if (sb < 0) packed |= HMJE_INPUT_SCROLL_DOWN_FLAG;
                legacy_input_push(packed);
            }
            goto logged;
        }
        long long ny = notches ? sb : (sb == 0 ? 0 : (sb > 0 ? 1 : -1) * ((sb > 0 ? sb : -sb) / 40 > 0 ? (sb > 0 ? sb : -sb) / 40 : 1));
        long long nx = notches ? sa : (sa == 0 ? 0 : (sa > 0 ? 1 : -1) * ((sa > 0 ? sa : -sa) / 40 > 0 ? (sa > 0 ? sa : -sa) / 40 : 1));
        POINT screen = {cursor_x, cursor_y};
        ClientToScreen(hwnd, &screen);
        LPARAM where = MAKELPARAM((WORD)(short)screen.x, (WORD)(short)screen.y);
        if (ny != 0) window_proc(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(ny * WHEEL_DELTA)), where);
        if (nx != 0) window_proc(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, (short)(-nx * WHEEL_DELTA)), where);
    } else if (strncmp(action, "trackpad:", 9) == 0) {
        double velocity = 0.0, finger_ms = 0.0, hz = 0.0;
        if (sscanf(action + 9, "%lf,%lf,%lf", &velocity, &finger_ms, &hz) >= 2)
            selftest_trackpad_start(hwnd, cursor_x, cursor_y, velocity, finger_ms, hz);
    } else if (sscanf(action, "key:%u,%7[csot]", &code, mods) >= 1) {
        selftest_key(hwnd, code, mods);
    } else if (strncmp(action, "ime:", 4) == 0) {
        g_st_comp_len = selftest_utf16(action + 4, g_st_comp, 512);
        if (!g_ime_composing) window_proc(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        window_proc(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_CURSORPOS);
    } else if (strncmp(action, "commit:", 7) == 0) {
        g_st_comp_len = 0;
        g_st_result_len = selftest_utf16(action + 7, g_st_result, 512);
        st_send_result(hwnd);
    } else if (strncmp(action, "text:", 5) == 0) {
        WCHAR units[256];
        size_t count = selftest_utf16(action + 5, units, 256);
        if (count > 0) committed_utf16_result_push(units, count);
    } else if (strncmp(action, "code:", 5) == 0) {
        /* code:B098,B2E4 — 유니코드 코드포인트(16진). 콘솔 코드 페이지를 거치는
         * 환경 변수로도 한글을 정확히 넘긴다. */
        WCHAR units[64];
        size_t count = 0;
        const char *cursor = action + 5;
        char *rest = NULL;
        while (*cursor != '\0' && count + 2 < 64) {
            unsigned long long value = strtoull(cursor, &rest, 16);
            if (rest == cursor) break;
            if (value >= 0x10000) {
                value -= 0x10000;
                units[count++] = (WCHAR)(0xd800 + (value >> 10));
                units[count++] = (WCHAR)(0xdc00 + (value & 0x3ff));
            } else {
                units[count++] = (WCHAR)value;
            }
            cursor = *rest == ',' ? rest + 1 : rest;
        }
        if (count > 0) committed_utf16_result_push(units, count);
    } else if (sscanf(action, "resize:%llux%llu", &a, &b) == 2) {
        /* Points, as the macOS host's setContentSize: (× surface scale). */
        selftest_resize(hwnd, a * g_scale, b * g_scale);
    } else if (strcmp(action, "close") == 0) {
        window_proc(hwnd, WM_CLOSE, 0, 0);
    } else if (strcmp(action, "ctrlw") == 0) {
        shortcut_window_key(hwnd, 'W');
    } else if (strcmp(action, "ctrlq") == 0 || strcmp(action, "quit") == 0) {
        shortcut_window_key(hwnd, 'Q');
    } else if (sscanf(action, "window:%llu", &a) == 1) {
        hmje_windows_window *target = hmje_window_for_id(a);
        if (target != NULL && !target->closing) {
            g_front_window = target;
            SetForegroundWindow(target->hwnd);
        }
    } else {
        fprintf(stderr, "windows-surface: selftest 모르는 동작 '%s'\n", action);
    }
logged:
    fprintf(stderr, "windows-selftest: %s (커서 %ld,%ld)\n", action, cursor_x, cursor_y);
}
