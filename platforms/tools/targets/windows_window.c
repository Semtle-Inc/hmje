/*
 * windows_window.c — SYS_FRAMEBUFFER_* / SYS_INPUT_POLL을 실제 Win32 창 + D3D11 스왑체인으로
 * 구현한다. windows_shim.c(순수 C, syscall 디스패치)가 extern으로 호출하는 4개 함수만
 * 외부에 노출한다(posix_host_shim.c ↔ posix_host_window.m과 같은 분리 원칙).
 *
 * 계약은 다른 hosted 백엔드와
 * 동일해야 한다:
 *   - blit(오프스크린 백버퍼 기록)과 present(commit)는 분리 — blit만으로는 화면에 안 보임.
 *   - 픽셀은 항상 canonical 0x00RRGGBB(리틀엔디안 자연수32 1픽셀, 메모리상 B,G,R,X).
 *
 * ── 표면 (GPU 렌더러) ────────────────────────
 * 표면은 D3D11 스왑체인(DXGI_FORMAT_B8G8R8A8_UNORM = canonical과 바이트 배치가 정확히
 * 같다 — Metal의 BGRA8Unorm과 동일한 이유로 스위즐 불필요, Android RGBX8888과 다른 점).
 * 기본 경로는 windows_d3d11_command_renderer.inc의 명령 버퍼/셰이더 실행이다.
 * GPU 작업 텍스처의 END만 retained staging과 스왑체인에 복사한다. CPU fallback은
 * 백버퍼를 STAGING 텍스처에 Map/memcpy한 뒤 표시하는 기존 계약을 유지한다.
 * WM_SIZE는 다음 메시지 펌프 끝에서 표면을 재생성하고, 입력보다 먼저
 * 공통 resize 이벤트를 전달한다. 최소화 중에는 마지막 유효 표면을 유지한다.
 *
 * D3D11CreateDeviceAndSwapChain은 D3D_DRIVER_TYPE_HARDWARE만 허용한다. WARP는 D3D
 * API를 쓰지만 최종 픽셀을 CPU가 만드는 software rasterizer라 production GPU
 * backend로 간주하지 않는다.
 *
 * hmje 쪽 그리기 루프는 자체 imperative 루프를 돌리므로 Win32 메시지 루프를 블로킹으로
 * 소유하지 않는다 — 창을 lazily 만들고, present/input poll 때마다 PeekMessage로
 * non-blocking 드레인만 한다(posix_host_window.m의 "[NSApp run]을 안 부른다" 원칙과 동일).
 */




/* The self-test script parser (windows_selftest.inc) uses sscanf on bounded, NUL-terminated
 * actions; without this every app build printed 17 MSVC CRT deprecation warnings. */
#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS /* d3d11.h의 C 호출 매크로(ID3D11Device_CreateTexture2D 등)를 활성화한다. */


#include <windows.h>
#include <oleidl.h>
#include <shellapi.h>
#include <imm.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pointer_input.h"
#include "legacy_input.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "normaliz.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "windowscodecs.lib")

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

typedef long long i64;
typedef unsigned long long u64;
#include "window_persist.h"
typedef struct HmjeDropRecord {
    struct HmjeDropRecord *next;
    size_t length;
    unsigned char bytes[];
} HmjeDropRecord;

#define HMJE_WINDOWS_ENOSYS_LOCAL (-38LL)

static u64 hmje_scale_for_dpi(UINT dpi);

#define DEFAULT_WIDTH 1180
#define DEFAULT_HEIGHT 860
#define MIN_CLIENT_WIDTH 320
#define MIN_CLIENT_HEIGHT 240

/* ---- 창 하나의 상태 ------------------------------------------------------
 * 창마다 한 구조체로 상태를 묶고, 지금 syscall을 부른 창 fiber의 창을 g_win이 가리킨다.
 * `#define g_width (HMJE_WIN->width)` 같은 별칭으로 현재 창의 상태를 읽고 쓴다.
 * window_proc은 메시지의 HWND로 창을 찾아 g_win을 잠깐 바꾼다(route).
 * 창이 없으면 모든 필드가 0인 g_null_win을 가리킨다(단위 시험의 가짜 HWND도 이쪽). */

typedef struct hmje_windows_window {
    struct hmje_windows_window *next;
    u64 id;
    u64 kind;
    HWND hwnd;
    BOOL ready, closed, resize_pending, closing, shown, primary, had_saved_frame, key_active;
    u64 acked_width, acked_height;
    u64 width, height, pitch;
    u64 scale; /* 정수 표면 배율(픽셀/포인트) — 모니터 DPI에서 반올림, 최소 1 */
    unsigned char *backbuffer; /* canonical 0x00RRGGBB, LE u32/px, malloc */
    IDXGISwapChain *swapchain;
    ID3D11Texture2D *backbuffer_tex;   /* 스왑체인 소유, DEFAULT usage */
    ID3D11Texture2D *staging_tex;      /* CPU 쓰기용, STAGING usage */
    BOOL has_presented_frame, repaint_pending;
    hmje_pointer_ring pointer_samples;
    hmje_legacy_input_ring legacy_input;
    WCHAR pending_high_surrogate;
    u64 text_input_request;
    BOOL ime_composing;
    BOOL ime_commit_return;
    u64 ime_result_count;
    u64 ime_keyups[4];
    BOOL caret_rect_valid;
    RECT caret_rect;
    IDropTarget *drop_target;
    BOOL drop_ole_initialized;
    HmjeDropRecord *drop_head, *drop_tail;
    unsigned drop_count;
    /* windows_d3d11_command_renderer.inc의 창별 대상 텍스처 두 장(HmjeD3DSurface[2]). */
    __declspec(align(16)) unsigned char d3d_targets[128];
} hmje_windows_window;

static hmje_windows_window g_null_win_storage;
static hmje_windows_window *g_win = NULL;
static hmje_windows_window *g_windows = NULL;
static u64 g_next_window_id = 1;
#define HMJE_WIN (g_win != NULL ? g_win : &g_null_win_storage)
#define g_hwnd (HMJE_WIN->hwnd)
#define g_ready (HMJE_WIN->ready)
#define g_closed (HMJE_WIN->closed)
#define g_resize_pending (HMJE_WIN->resize_pending)
#define g_acked_width (HMJE_WIN->acked_width)
#define g_acked_height (HMJE_WIN->acked_height)
#define g_width (HMJE_WIN->width)
#define g_height (HMJE_WIN->height)
#define g_pitch (HMJE_WIN->pitch)
#define g_scale (HMJE_WIN->scale != 0 ? HMJE_WIN->scale : 1ull)
#define g_backbuffer (HMJE_WIN->backbuffer)
#define g_swapchain (HMJE_WIN->swapchain)
#define g_backbuffer_tex (HMJE_WIN->backbuffer_tex)
#define g_staging_tex (HMJE_WIN->staging_tex)
#define g_has_presented_frame (HMJE_WIN->has_presented_frame)
#define g_repaint_pending (HMJE_WIN->repaint_pending)
#define g_pointer_samples (HMJE_WIN->pointer_samples)
#define g_legacy_input (HMJE_WIN->legacy_input)
#define g_pending_high_surrogate (HMJE_WIN->pending_high_surrogate)
#define g_text_input_request (HMJE_WIN->text_input_request)
#define g_ime_composing (HMJE_WIN->ime_composing)
#define g_ime_commit_return (HMJE_WIN->ime_commit_return)
#define g_ime_result_count (HMJE_WIN->ime_result_count)
#define g_ime_keyups (HMJE_WIN->ime_keyups)
#define g_caret_rect_valid (HMJE_WIN->caret_rect_valid)
#define g_caret_rect (HMJE_WIN->caret_rect)

static void *hmje_windows_d3d_targets(void) {
    return HMJE_WIN->d3d_targets;
}

static hmje_windows_window *hmje_window_for_hwnd(HWND hwnd) {
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
        if (window->hwnd == hwnd && hwnd != NULL) return window;
    return NULL;
}

static hmje_windows_window *hmje_window_for_id(u64 id) {
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
        if (window->id == id) return window;
    return NULL;
}

/* OS -> app only. Drag sources remain entirely inside HmjeUI. */
enum { HMJE_DROP_FILE = 1, HMJE_DROP_TEXT = 2, HMJE_DROP_IMAGE = 3 };
enum { HMJE_DROP_ENTER = 1, HMJE_DROP_MOVE = 2, HMJE_DROP_LEAVE = 3, HMJE_DROP_FINISH = 4 };
typedef struct HmjeDropTarget {
    IDropTarget iface;
    LONG references;
    hmje_windows_window *window;
    u64 kind, x, y;
    UINT png_format;
    CLIPFORMAT image_format;
} HmjeDropTarget;
static void windows_drop_enqueue(HmjeDropTarget *target, u64 phase,
                                 const unsigned char *payload, size_t length) {
    hmje_windows_window *window = target->window;
    if (!target->kind || !window || !window->id || length > (8u << 20)) return;
    HmjeDropRecord *record = (HmjeDropRecord *)malloc(sizeof(*record) + 48 + length);
    if (!record) return;
    record->next = NULL; record->length = 48 + length;
    u64 header[6] = {target->kind, phase, target->x, target->y, window->id, length};
    memcpy(record->bytes, header, 48);
    if (length) memcpy(record->bytes + 48, payload, length);
    if (window->drop_tail) window->drop_tail->next = record;
    else window->drop_head = record;
    window->drop_tail = record;
    window->drop_count++;
    if (window->drop_count > 256) {
        HmjeDropRecord *old = window->drop_head;
        window->drop_head = old->next;
        free(old); window->drop_count--;
    }
}
static void windows_drop_point(HmjeDropTarget *target, POINTL screen) {
    POINT point = {(LONG)screen.x, (LONG)screen.y};
    ScreenToClient(target->window->hwnd, &point);
    target->x = point.x < 0 ? 0 : (u64)point.x;
    target->y = point.y < 0 ? 0 : (u64)point.y;
}
static int windows_drop_format(IDataObject *object, CLIPFORMAT format) {
    FORMATETC request = {format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SUCCEEDED(IDataObject_QueryGetData(object, &request));
}
static unsigned char *windows_drop_wic_png(const void *bytes, size_t length,
                                            int dib, size_t *output_length) {
    if (!bytes || !length || length > (8u << 20)) return NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *source = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    IStream *input = NULL, *output = NULL;
    unsigned char *result = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) goto done;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &input))) goto done;
    if (dib) {
        if (length < sizeof(BITMAPINFOHEADER)) goto done;
        const BITMAPINFOHEADER *info = (const BITMAPINFOHEADER *)bytes;
        if (info->biSize < sizeof(BITMAPINFOHEADER) || info->biSize > length) goto done;
        size_t colors = info->biClrUsed ? info->biClrUsed :
            info->biBitCount <= 8 ? (size_t)1 << info->biBitCount : 0;
        size_t offset = 14 + info->biSize + colors * 4;
        if (info->biSize == sizeof(BITMAPINFOHEADER) && info->biCompression == BI_BITFIELDS)
            offset += 12;
        if (offset > length + 14) goto done;
        BITMAPFILEHEADER header = {0};
        header.bfType = 0x4d42;
        header.bfSize = (DWORD)(length + 14);
        header.bfOffBits = (DWORD)offset;
        if (FAILED(IStream_Write(input, &header, sizeof(header), NULL))) goto done;
    }
    if (FAILED(IStream_Write(input, bytes, (ULONG)length, NULL))) goto done;
    LARGE_INTEGER zero = {0};
    if (FAILED(IStream_Seek(input, zero, STREAM_SEEK_SET, NULL))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(factory, input, NULL,
            WICDecodeMetadataCacheOnDemand, &decoder))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &source))) goto done;
    UINT width = 0, height = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(source, &width, &height)) ||
        !width || !height || width > 8192 || height > 8192 ||
        (uint64_t)width * height > 16000000) goto done;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &output))) goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng,
            NULL, &encoder))) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(encoder, output, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, options))) goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, width, height))) goto done;
    WICPixelFormatGUID pixel = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel))) goto done;
    if (FAILED(IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)source, NULL))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)) ||
        FAILED(IWICBitmapEncoder_Commit(encoder))) goto done;
    HGLOBAL global = NULL;
    if (FAILED(GetHGlobalFromStream(output, &global))) goto done;
    size_t size = GlobalSize(global);
    if (!size || size > (8u << 20)) goto done;
    const void *encoded = GlobalLock(global);
    if (!encoded) goto done;
    result = malloc(size);
    if (result) { memcpy(result, encoded, size); *output_length = size; }
    GlobalUnlock(global);
done:
    if (options) IPropertyBag2_Release(options);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (source) IWICBitmapFrameDecode_Release(source);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    if (input) IStream_Release(input);
    if (output) IStream_Release(output);
    return result;
}
static u64 windows_drop_kind(HmjeDropTarget *target, IDataObject *object) {
    target->image_format = 0;
    if (windows_drop_format(object, CF_HDROP)) return HMJE_DROP_FILE;
    const wchar_t *names[] = {L"PNG", L"image/png", L"JFIF", L"image/jpeg",
        L"image/bmp", L"image/gif", L"GIF", L"image/tiff", L"TIFF", L"image/webp"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        UINT format = RegisterClipboardFormatW(names[i]);
        if (format && windows_drop_format(object, (CLIPFORMAT)format)) {
            target->image_format = (CLIPFORMAT)format;
            return HMJE_DROP_IMAGE;
        }
    }
    if (windows_drop_format(object, CF_DIBV5)) {
        target->image_format = CF_DIBV5; return HMJE_DROP_IMAGE;
    }
    if (windows_drop_format(object, CF_DIB)) {
        target->image_format = CF_DIB; return HMJE_DROP_IMAGE;
    }
    if (windows_drop_format(object, CF_UNICODETEXT)) return HMJE_DROP_TEXT;
    return 0;
}
static unsigned char *windows_drop_utf8(const wchar_t *wide, size_t *length) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (count <= 0) return NULL;
    unsigned char *text = (unsigned char *)malloc((size_t)count);
    if (!text) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, (char *)text, count, NULL, NULL)) {
        free(text); return NULL;
    }
    *length = (size_t)count - 1;
    return text;
}
static unsigned char *windows_drop_payload(HmjeDropTarget *target, IDataObject *object,
                                           size_t *length) {
    CLIPFORMAT format = target->kind == HMJE_DROP_FILE ? CF_HDROP :
        target->kind == HMJE_DROP_IMAGE ? target->image_format : CF_UNICODETEXT;
    FORMATETC request = {format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {0};
    if (FAILED(IDataObject_GetData(object, &request, &medium))) return NULL;
    unsigned char *result = NULL;
    if (target->kind == HMJE_DROP_FILE) {
        UINT count = DragQueryFileW((HDROP)medium.hGlobal, 0xffffffffu, NULL, 0);
        size_t used = 0, capacity = 0;
        for (UINT i = 0; i < count; i++) {
            UINT chars = DragQueryFileW((HDROP)medium.hGlobal, i, NULL, 0);
            wchar_t *path = (wchar_t *)calloc((size_t)chars + 1, sizeof(wchar_t));
            if (!path) break;
            DragQueryFileW((HDROP)medium.hGlobal, i, path, chars + 1);
            int normalized_count = NormalizeString(NormalizationC, path, -1, NULL, 0);
            if (normalized_count > 0) {
                wchar_t *normalized = (wchar_t *)calloc((size_t)normalized_count + 1, sizeof(wchar_t));
                if (normalized && NormalizeString(NormalizationC, path, -1,
                                                   normalized, normalized_count) > 0) {
                    free(path); path = normalized;
                } else free(normalized);
            }
            size_t bytes = 0;
            unsigned char *utf8 = windows_drop_utf8(path, &bytes);
            free(path);
            if (!utf8 || bytes == 0 || used + bytes + 1 > (8u << 20)) { free(utf8); continue; }
            if (used + bytes + 1 > capacity) {
                size_t next_capacity = (used + bytes + 1) * 2;
                unsigned char *grown = (unsigned char *)realloc(result, next_capacity);
                if (!grown) { free(utf8); break; }
                result = grown; capacity = next_capacity;
            }
            memcpy(result + used, utf8, bytes); used += bytes; result[used++] = 0;
            free(utf8);
        }
        *length = used;
    } else if (target->kind == HMJE_DROP_TEXT) {
        const wchar_t *wide = (const wchar_t *)GlobalLock(medium.hGlobal);
        if (wide) {
            size_t max_chars = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
            if (max_chars && wmemchr(wide, 0, max_chars))
                result = windows_drop_utf8(wide, length);
            GlobalUnlock(medium.hGlobal);
        }
    } else {
        size_t bytes = GlobalSize(medium.hGlobal);
        const void *data = GlobalLock(medium.hGlobal);
        if (data && bytes > 0 && bytes <= (8u << 20)) {
            const unsigned char *image = data;
            if ((bytes >= 8 && !memcmp(image, "\x89PNG\r\n\x1a\n", 8)) ||
                (bytes >= 2 && image[0] == 0xff && image[1] == 0xd8)) {
                result = (unsigned char *)malloc(bytes);
                if (result) { memcpy(result, data, bytes); *length = bytes; }
            } else {
                result = windows_drop_wic_png(data, bytes,
                    format == CF_DIB || format == CF_DIBV5, length);
            }
        }
        if (data) GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}
static HRESULT STDMETHODCALLTYPE drop_query(IDropTarget *iface, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IDropTarget)) return E_NOINTERFACE;
    *out = iface;
    InterlockedIncrement(&((HmjeDropTarget *)iface)->references);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE drop_addref(IDropTarget *iface) {
    return (ULONG)InterlockedIncrement(&((HmjeDropTarget *)iface)->references);
}
static ULONG STDMETHODCALLTYPE drop_release(IDropTarget *iface) {
    HmjeDropTarget *target = (HmjeDropTarget *)iface;
    LONG remaining = InterlockedDecrement(&target->references);
    if (!remaining) free(target);
    return (ULONG)remaining;
}
static HRESULT STDMETHODCALLTYPE drop_enter(IDropTarget *iface, IDataObject *object,
                                           DWORD keys, POINTL point, DWORD *effect) {
    (void)keys;
    HmjeDropTarget *target = (HmjeDropTarget *)iface;
    target->kind = windows_drop_kind(target, object);
    windows_drop_point(target, point);
    windows_drop_enqueue(target, HMJE_DROP_ENTER, NULL, 0);
    *effect = target->kind ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE drop_over(IDropTarget *iface, DWORD keys,
                                          POINTL point, DWORD *effect) {
    (void)keys;
    HmjeDropTarget *target = (HmjeDropTarget *)iface;
    windows_drop_point(target, point);
    windows_drop_enqueue(target, HMJE_DROP_MOVE, NULL, 0);
    *effect = target->kind ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE drop_leave(IDropTarget *iface) {
    HmjeDropTarget *target = (HmjeDropTarget *)iface;
    windows_drop_enqueue(target, HMJE_DROP_LEAVE, NULL, 0);
    target->kind = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE drop_finish(IDropTarget *iface, IDataObject *object,
                                            DWORD keys, POINTL point, DWORD *effect) {
    (void)keys;
    HmjeDropTarget *target = (HmjeDropTarget *)iface;
    windows_drop_point(target, point);
    size_t length = 0;
    unsigned char *payload = windows_drop_payload(target, object, &length);
    *effect = payload ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (payload) windows_drop_enqueue(target, HMJE_DROP_FINISH, payload, length);
    free(payload); target->kind = 0;
    return S_OK;
}
static IDropTargetVtbl g_drop_vtable = {
    drop_query, drop_addref, drop_release, drop_enter, drop_over, drop_leave, drop_finish
};
static void windows_drop_register(hmje_windows_window *window) {
    if (FAILED(OleInitialize(NULL))) return;
    window->drop_ole_initialized = TRUE;
    HmjeDropTarget *target = (HmjeDropTarget *)calloc(1, sizeof(*target));
    if (!target) return;
    target->iface.lpVtbl = &g_drop_vtable;
    target->references = 1;
    target->window = window;
    target->png_format = RegisterClipboardFormatW(L"PNG");
    if (SUCCEEDED(RegisterDragDrop(window->hwnd, &target->iface))) window->drop_target = &target->iface;
    else IDropTarget_Release(&target->iface);
}

i64 hmje_windows_ui_drag_drop(i64 operation, i64 address, i64 capacity) {
    hmje_windows_window *window = HMJE_WIN;
    HmjeDropRecord *head = window->drop_head;
    if (operation == 0) return head ? (i64)head->length : 0;
    if (operation != 1) return -22;
    if (!head) return 0;
    if (!address || capacity < (i64)head->length) return -34;
    memcpy((void *)(uintptr_t)(u64)address, head->bytes, head->length);
    i64 length = (i64)head->length;
    window->drop_head = head->next;
    if (!window->drop_head) window->drop_tail = NULL;
    window->drop_count--;
    free(head);
    return length;
}

/* 창 사건(KIND_WINDOW, 운영체제 계약.hmje INPUT_WINDOW_EVENT_*). */
#define HMJE_INPUT_KIND_WINDOW 9ull
#define HMJE_WINDOW_EVENT_CLOSE_REQUEST 1ull
#define HMJE_WINDOW_EVENT_BECAME_KEY 2ull
#define HMJE_WINDOW_EVENT_RESIGNED_KEY 3ull
#define HMJE_WINDOW_EVENT_CLOSE 4ull
#define HMJE_WINDOW_EVENT_QUIT_FLAG 256ull
/* HmjeUI가 KIND_WINDOW 사건을 다룬다고 알렸다(UI_WINDOW_OP_QUERY a2 bit0). 알리지
 * 않은 앱은 닫기 = 창 파괴 = APP_CLOSE다. */
static BOOL g_window_events_aware = FALSE;
static BOOL g_quit_requested = FALSE;
/* 마지막 창이 닫힌 뒤의 정책(WINDOW_POLICY_*). Windows 기본은 앱 종료. */
static u64 g_last_window_policy = 1;
static void window_push_event(hmje_windows_window *window, u64 kind_event);
static BOOL g_selftest_active; /* windows_selftest.inc */

/* Reapplying the same caption sends WM_SETTEXT/non-client redraws every frame. */
static BOOL hmje_set_window_title(HWND hwnd, const wchar_t *title) {
    int length = GetWindowTextLengthW(hwnd);
    if ((size_t)length == wcslen(title)) {
        wchar_t stack[256];
        wchar_t *current = length < 256 ? stack : (wchar_t *)calloc((size_t)length + 1, sizeof(wchar_t));
        if (current != NULL) {
            GetWindowTextW(hwnd, current, length + 1);
            BOOL same = wcscmp(current, title) == 0;
            if (current != stack) free(current);
            if (same) return TRUE;
        }
    }
    return SetWindowTextW(hwnd, title);
}

/* Stored physical frames can outlive a VM display/DPI change. */
static void hmje_constrain_window_frame(RECT *frame) {
    MONITORINFO monitor = {sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(frame, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    RECT work = monitor.rcWork;
    i64 width = (i64)frame->right - frame->left, height = (i64)frame->bottom - frame->top;
    if (width > work.right - work.left) width = work.right - work.left;
    if (height > work.bottom - work.top) height = work.bottom - work.top;
    if (frame->left > work.right - width) frame->left = work.right - width;
    if (frame->top > work.bottom - height) frame->top = work.bottom - height;
    if (frame->left < work.left) frame->left = work.left;
    if (frame->top < work.top) frame->top = work.top;
    frame->right = frame->left + width;
    frame->bottom = frame->top + height;
}

static void hmje_restore_window_frame(HWND hwnd, int x, int y, int width, int height) {
    if (width < 1 || height < 1) return;
    RECT frame = {x, y, (LONG)min((i64)x + width, 2147483647LL),
                       (LONG)min((i64)y + height, 2147483647LL)};
    hmje_constrain_window_frame(&frame);
    SetWindowPos(hwnd, NULL, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

i64 hmje_windows_ui_window_config(i64 title_address, i64 title_length, i64 flags) {
    if (g_hwnd == NULL || title_length < 0 || title_length > INT_MAX || (title_length > 0 && title_address == 0)) return -38;
    if (title_length > 0) {
        const char *utf8 = (const char *)(uintptr_t)(u64)title_address;
        int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)title_length, NULL, 0);
        if (wide_length <= 0) return -22;
        wchar_t *wide = (wchar_t *)calloc((size_t)wide_length + 1u, sizeof(wchar_t));
        if (wide == NULL) return -12;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)title_length, wide, wide_length) <= 0) {
            free(wide);
            return -22;
        }
        hmje_set_window_title(g_hwnd, wide);
        free(wide);
    }
    LONG_PTR previous_style = GetWindowLongPtrW(g_hwnd, GWL_STYLE), style = previous_style;
    if (((u64)flags & 1u) != 0) style &= ~WS_CAPTION;
    else style |= WS_CAPTION;
    if (style != previous_style) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, style);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return 0;
}
/* Like the macOS surface, geometry is acknowledged by INPUT_POLL, not by a
 * bounded event queue. A pointer storm cannot discard the only resize notice. */


static void hmje_d3d_resize_invalidate(void);

static ID3D11Device *g_device = NULL;
static ID3D11DeviceContext *g_context = NULL;
/* The staging texture retains the last committed frame; g_backbuffer may
 * already contain newer blits that must remain invisible until present. */
static i64 present_staging_frame(void);

#define HMJE_INPUT_KIND_SHIFT HMJE_LEGACY_INPUT_KIND_SHIFT
#define HMJE_INPUT_KIND_POINTER_MOVE HMJE_LEGACY_INPUT_KIND_POINTER_MOVE
#define HMJE_INPUT_KIND_POINTER_BUTTON HMJE_LEGACY_INPUT_KIND_POINTER_BUTTON
#define HMJE_INPUT_KIND_POINTER_SCROLL 3ull
#define HMJE_INPUT_KIND_APP_CLOSE 6ull
#define HMJE_INPUT_POINTER_COORD_MASK 16777215ull
#define HMJE_INPUT_POINTER_Y_SHIFT 24
#define HMJE_INPUT_POINTER_BUTTON_SHIFT 48
#define HMJE_INPUT_POINTER_PRESSED_FLAG (1ull << 55)
#define HMJE_INPUT_SCROLL_DOWN_FLAG (1ull << 55)

/* 현재 HWND 생성/메시지 펌프/SYS_INPUT_POLL은 같은 HMJE thread에서 돌지만,
 * SendMessage 계열이나 향후 전용 UI thread로 소유권이 바뀌어도 동적 성장 중
 * push/pop이 경쟁하지 않도록 큐 자체의 동시성 계약을 명시적으로 지킨다. */
static SRWLOCK g_legacy_lock = SRWLOCK_INIT;

static void legacy_input_push(u64 value) {
    AcquireSRWLockExclusive(&g_legacy_lock);
    hmje_legacy_input_ring_push(&g_legacy_input, value);
    ReleaseSRWLockExclusive(&g_legacy_lock);
}

static i64 legacy_input_pop(void) {
    AcquireSRWLockExclusive(&g_legacy_lock);
    i64 value = hmje_legacy_input_ring_pop(&g_legacy_input);
    ReleaseSRWLockExclusive(&g_legacy_lock);
    return value;
}

static BOOL windows_debug_enabled(void) {
    /* The macOS self-test names (HMJE_POSIX_*) are accepted too, as for the script
     * and the frame dumps, so the same window-order checks read both hosts. */
    return GetEnvironmentVariableA("HMJE_WINDOWS_DEBUG_WINDOWS", NULL, 0) > 0
        || GetEnvironmentVariableA("HMJE_WINDOWS_SELFTEST", NULL, 0) > 0
        || GetEnvironmentVariableA("HMJE_POSIX_DEBUG_WINDOWS", NULL, 0) > 0
        || GetEnvironmentVariableA("HMJE_POSIX_SELFTEST_POINTER", NULL, 0) > 0;
}

static void window_push_event(hmje_windows_window *window, u64 kind_event) {
    if (window == NULL) return;
    AcquireSRWLockExclusive(&g_legacy_lock);
    hmje_legacy_input_ring_push(&window->legacy_input, (HMJE_INPUT_KIND_WINDOW << HMJE_LEGACY_INPUT_KIND_SHIFT) | kind_event);
    ReleaseSRWLockExclusive(&g_legacy_lock);
    if (windows_debug_enabled())
        fprintf(stderr, "windows-window: event id=%llu kind=%llu\n", (unsigned long long)window->id,
                (unsigned long long)kind_event);
}

static void request_application_quit(void) {
    g_quit_requested = TRUE;
    if (windows_debug_enabled()) fprintf(stderr, "windows-window: quit requested\n");
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
        if (!window->closing) window_push_event(window, HMJE_WINDOW_EVENT_CLOSE_REQUEST | HMJE_WINDOW_EVENT_QUIT_FLAG);
}


/* ---- IME (IMM32) -------------------------------------------------------
 * The host draws nothing for the IME. Committed text becomes KIND_TEXT, the
 * syllable being composed becomes KIND_COMPOSITION (code points + END with the
 * caret), and HMJE draws it at its caret with an underline. The IME's own
 * composition window is therefore suppressed (WM_IME_SETCONTEXT/STARTCOMPOSITION)
 * and its candidate window is placed at the rectangle HMJE reports through
 * SYS_TEXT_INPUT_CONTROL2(2, ...).
 *
 * Keys the IME handles arrive as WM_KEYDOWN VK_PROCESSKEY (recorded with the
 * Korean IME: every jamo, and Backspace inside a syllable). They are not keys
 * for HMJE: forwarding them made Backspace delete the previous character too.
 * A key that ends the composition (Space, Return, arrows) arrives after the
 * IME's result as an ordinary WM_KEYDOWN and is forwarded after the text.
 * A composition-commit Return carries bit 21 so opted-in Enter actions can
 * consume it without changing ordinary inputs.
 *
 * The IMM calls go through g_imm_test when a unit test installs fakes
 * (platforms/tests/windows-ime/compose.c). */
typedef struct hmje_imm_hooks {
    HIMC (*get_context)(HWND window);
    BOOL (*release_context)(HWND window, HIMC context);
    LONG (*get_composition)(HIMC context, DWORD index, LPVOID buffer, DWORD bytes);
    BOOL (*notify)(HIMC context, DWORD action, DWORD index, DWORD value);
    UINT (*get_virtual_key)(HWND window);
    BOOL (*set_composition_window)(HIMC context, COMPOSITIONFORM *form);
    BOOL (*set_candidate_window)(HIMC context, CANDIDATEFORM *form);
} hmje_imm_hooks;
static const hmje_imm_hooks *g_imm_test = NULL;

static HIMC imm_get_context(HWND window) {
    return g_imm_test ? g_imm_test->get_context(window) : ImmGetContext(window);
}
static void imm_release_context(HWND window, HIMC context) {
    if (g_imm_test) g_imm_test->release_context(window, context);
    else ImmReleaseContext(window, context);
}
static LONG imm_get_composition(HIMC context, DWORD index, LPVOID buffer, DWORD bytes) {
    return g_imm_test ? g_imm_test->get_composition(context, index, buffer, bytes)
                      : ImmGetCompositionStringW(context, index, buffer, bytes);
}
static BOOL imm_notify(HIMC context, DWORD action, DWORD index, DWORD value) {
    return g_imm_test ? g_imm_test->notify(context, action, index, value)
                      : ImmNotifyIME(context, action, index, value);
}
static UINT imm_get_virtual_key(HWND window) {
    return g_imm_test ? g_imm_test->get_virtual_key(window) : ImmGetVirtualKey(window);
}

#define HMJE_INPUT_KIND_COMPOSITION 8ull
#define HMJE_INPUT_COMPOSITION_END_FLAG 4194304ull
#define HMJE_INPUT_COMPOSITION_CARET_MASK 65535ull
#define HMJE_INPUT_KEY_MOD_PRIMARY (1ull << 20)

/* While the host itself ends a composition, the IME's answer is not input. */
static unsigned g_ime_ending = 0;
/* A preedit is on HMJE's screen (the last update had code points). */
/* Scan codes whose down went to the IME: their key-ups stay there too. */

#define HMJE_INPUT_KIND_EDIT_COMMAND 7ULL
#define HMJE_EDIT_SELECT_ALL 1ULL
#define HMJE_EDIT_COPY 2ULL
#define HMJE_EDIT_CUT 3ULL
#define HMJE_EDIT_PASTE 4ULL
#define HMJE_EDIT_UNDO 5ULL
#define HMJE_EDIT_REDO 6ULL

static void edit_command_push(u64 command) {
    legacy_input_push((HMJE_INPUT_KIND_EDIT_COMMAND << HMJE_INPUT_KIND_SHIFT)
                      | (command & 255ULL));
}

static void end_composition(HWND hwnd, BOOL commit);

/* Ctrl+W = 이 창 닫기 요청, Ctrl+Q = 앱 종료 요청(HmjeUI 주 단축키 `명령`이 Ctrl).
 * 창 사건을 아는 앱은 거부할 수 있다(저장 안 한 변경). 창 사건을 모르는 앱은 창을 부순다. */
static void shortcut_window_key(HWND hwnd, WPARAM key) {
    if (!g_window_events_aware) DestroyWindow(hwnd);
    else if (key == 'W') window_push_event(HMJE_WIN, HMJE_WINDOW_EVENT_CLOSE_REQUEST);
    else request_application_quit();
}

static BOOL handle_windows_shortcut(HWND hwnd, UINT msg, WPARAM key, LPARAM detail) {
    BOOL down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return FALSE;
    /* A shortcut ends the syllable the way a click does, so Ctrl+S saves it
     * and Ctrl+Z undoes it as a whole. */
    if (down && key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU && key != VK_PROCESSKEY)
        end_composition(hwnd, TRUE);
    if (key == 'Q' || key == 'W') {
        if (down && !(detail & (1LL << 30))) shortcut_window_key(hwnd, key);
        return TRUE;
    }
    u64 command = 0;
    if (key == 'A') command = HMJE_EDIT_SELECT_ALL;
    else if (key == 'C') command = HMJE_EDIT_COPY;
    else if (key == 'X') command = HMJE_EDIT_CUT;
    else if (key == 'V') command = HMJE_EDIT_PASTE;
    else if (key == 'Y') command = HMJE_EDIT_REDO;
    else if (key == 'Z') command = (GetKeyState(VK_SHIFT) & 0x8000) ? HMJE_EDIT_REDO : HMJE_EDIT_UNDO;
    if (command == 0) return FALSE;
    if (down && !(detail & (1LL << 30))) edit_command_push(command);
    return TRUE;
}

static BOOL committed_scalar_push_flag(u64 scalar, BOOL continuation) {
    /* Editing controls are delivered only as physical keys. WM_CHAR can also
     * carry Ctrl+A/backspace/return; inserting these would duplicate edits. */
    if (scalar < 32 || scalar == 127 || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff)) return FALSE;
    legacy_input_push((4ull << HMJE_INPUT_KIND_SHIFT) | scalar
                      | (continuation ? 2097152ull : 0ull));
    return TRUE;
}

static void committed_scalar_push(u64 scalar) {
    (void)committed_scalar_push_flag(scalar, FALSE);
}

/* GCS_RESULTSTR is one IME commit, unlike separate WM_CHAR messages. Decode
 * UTF-16 as Unicode scalars and mark all but the first for a single undo step. */
static void committed_utf16_result_push(const WCHAR *units, size_t count) {
    BOOL emitted = FALSE;
    for (size_t i = 0; i < count; ++i) {
        u64 scalar = units[i];
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
            if (i + 1 < count && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
                scalar = 0x10000 + ((scalar - 0xd800) << 10) + (units[++i] - 0xdc00);
            } else scalar = 0xfffd;
        } else if (scalar >= 0xdc00 && scalar <= 0xdfff) scalar = 0xfffd;
        if (committed_scalar_push_flag(scalar, emitted)) emitted = TRUE;
    }
}

/* One KIND_COMPOSITION update: code points, then END with the caret (code
 * points into the string). units == NULL/count == 0 ends the preedit. */
static void composition_utf16_push(const WCHAR *units, size_t count, size_t caret_units) {
    u64 caret = 0;
    for (size_t i = 0; i < count; ++i) {
        u64 scalar = units[i];
        size_t start = i;
        if (scalar >= 0xd800 && scalar <= 0xdbff && i + 1 < count
            && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (units[++i] - 0xdc00);
        } else if (scalar >= 0xd800 && scalar <= 0xdfff) scalar = 0xfffd;
        if (start < caret_units) caret++;
        legacy_input_push((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT) | scalar);
    }
    if (caret > HMJE_INPUT_COMPOSITION_CARET_MASK) caret = HMJE_INPUT_COMPOSITION_CARET_MASK;
    legacy_input_push((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT)
                      | HMJE_INPUT_COMPOSITION_END_FLAG | caret);
    g_ime_composing = count > 0;
}

/* Read one IMM string (GCS_COMPSTR/GCS_RESULTSTR) into a malloc'd buffer. */
static WCHAR *imm_string(HIMC context, DWORD index, size_t *count) {
    *count = 0;
    LONG bytes = imm_get_composition(context, index, NULL, 0);
    if (bytes <= 0 || bytes > 1024 * 1024 || (bytes % sizeof(WCHAR)) != 0) return NULL;
    WCHAR *units = (WCHAR *)malloc((size_t)bytes);
    if (!units) return NULL;
    if (imm_get_composition(context, index, units, (DWORD)bytes) != bytes) { free(units); return NULL; }
    *count = (size_t)bytes / sizeof(WCHAR);
    return units;
}

/* WM_IME_COMPOSITION with GCS_COMPSTR: the preedit changed (possibly to empty,
 * e.g. Backspace on the last jamo cancels it). The Korean IME reports
 * CS_INSERTCHAR with its cursor at 0 (a block over the syllable); the caret
 * then belongs after the syllable, where the next jamo goes. */
static void composition_update_push(HWND hwnd, LPARAM flags) {
    HIMC context = imm_get_context(hwnd);
    if (!context) return;
    size_t count = 0;
    WCHAR *units = imm_string(context, GCS_COMPSTR, &count);
    size_t caret = count;
    if (!(flags & CS_INSERTCHAR) && (flags & GCS_CURSORPOS)) {
        LONG cursor = imm_get_composition(context, GCS_CURSORPOS, NULL, 0);
        if (cursor >= 0 && (size_t)cursor < count) caret = (size_t)cursor;
    }
    imm_release_context(hwnd, context);
    if (count > 0 || g_ime_composing) composition_utf16_push(units, count, caret);
    free(units);
}

static BOOL committed_ime_result_push(HWND hwnd) {
    HIMC context = imm_get_context(hwnd);
    if (!context) return FALSE;
    LONG bytes = imm_get_composition(context, GCS_RESULTSTR, NULL, 0);
    /* A malformed or unreasonably large result falls back to the OS default
     * path, which can still deliver WM_CHAR; never silently lose a commit. */
    if (bytes < 0 || bytes > 1024 * 1024 || (bytes % sizeof(WCHAR)) != 0) {
        imm_release_context(hwnd, context);
        return FALSE;
    }
    WCHAR *units = bytes ? (WCHAR *)malloc((size_t)bytes) : NULL;
    if (bytes && !units) { imm_release_context(hwnd, context); return FALSE; }
    LONG read = bytes ? imm_get_composition(context, GCS_RESULTSTR, units, (DWORD)bytes) : 0;
    imm_release_context(hwnd, context);
    if (read != bytes) { free(units); return FALSE; }
    g_pending_high_surrogate = 0;
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) g_ime_commit_return = TRUE;
    committed_utf16_result_push(units, (size_t)bytes / sizeof(WCHAR));
    free(units);
    g_ime_result_count++;
    /* Committed text replaces the preedit for HMJE; an empty result must
     * still take it off the screen. */
    if (bytes == 0 && g_ime_composing) composition_utf16_push(NULL, 0, 0);
    g_ime_composing = FALSE;
    return TRUE;
}

static void committed_utf16_push(WCHAR unit) {
    if (unit >= 0xd800 && unit <= 0xdbff) {
        if (g_pending_high_surrogate) committed_scalar_push(0xfffd);
        g_pending_high_surrogate = unit;
        return;
    }
    if (unit >= 0xdc00 && unit <= 0xdfff) {
        if (g_pending_high_surrogate) {
            committed_scalar_push(0x10000 + ((u64)(g_pending_high_surrogate - 0xd800) << 10)
                                  + (unit - 0xdc00));
            g_pending_high_surrogate = 0;
        } else committed_scalar_push(0xfffd);
        return;
    }
    if (g_pending_high_surrogate) committed_scalar_push(0xfffd);
    g_pending_high_surrogate = 0;
    committed_scalar_push(unit);
}

static u64 windows_key_event(WPARAM key, LPARAM detail, BOOL down) {
    u64 scan = ((u64)detail >> 16) & 255;
    if (scan == 0) scan = MapVirtualKeyW((UINT)key, MAPVK_VK_TO_VSC) & 255;
    /* Hosted control aliases avoid collisions in the UI's shared macOS/set-1
     * editor. In particular set-1 comma is macOS backspace; hosted Caps Lock
     * must not enable the QEMU-only Korean keyboard composer. */
    if (key == VK_TAB) scan = 250;
    else if (key == VK_RETURN) scan = 251;
    else if (key == VK_SPACE) scan = 252;
    else if (key == VK_OEM_COMMA) scan = 253;
    else if (key == VK_CAPITAL) scan = 254;
    /* Set-1 slash is 53, also the shared UI's macOS Escape code.
     * Its printable text still arrives separately through WM_CHAR. */
    else if (key == VK_OEM_2 || key == VK_DIVIDE) scan = 255;
    u64 value = scan | (down ? 256ull : 0);
    if (GetKeyState(VK_SHIFT) & 0x8000) value |= 1ull << 16;
    /* Control is the primary shortcut modifier here (`명령 S키` = Ctrl+S).
     * AltGr is reported as Ctrl+Alt; it types characters, not shortcuts. */
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        value |= 1ull << 17;
        if (!(GetKeyState(VK_RMENU) & 0x8000)) value |= HMJE_INPUT_KEY_MOD_PRIMARY;
    }
    if (GetKeyState(VK_MENU) & 0x8000) value |= 1ull << 18;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) value |= 1ull << 19;
    return value;
}

/* Finish the composition from the host side, like macOS end_composition:
 * commit=TRUE (a click, a Ctrl shortcut) completes it where the caret is now,
 * before the press/shortcut is queued; commit=FALSE (focus moved in HMJE)
 * drops it. The IME is always told, or it keeps composing a syllable HMJE no
 * longer shows. If the IME does not answer CPS_COMPLETE with a result right
 * away, the host commits the preedit it read and cancels the IME's copy. */
static void end_composition(HWND hwnd, BOOL commit) {
    if (!hwnd) return;
    HIMC context = imm_get_context(hwnd);
    if (!context) {
        if (g_ime_composing) composition_utf16_push(NULL, 0, 0);
        return;
    }
    size_t count = 0;
    WCHAR *pending = imm_string(context, GCS_COMPSTR, &count);
    if (count > 0 && commit) {
        u64 before = g_ime_result_count;
        imm_notify(context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
        if (!g_imm_test) {
            /* Some IMEs post rather than send their answer. */
            MSG message;
            while (PeekMessageW(&message, hwnd, WM_IME_STARTCOMPOSITION, WM_IME_KEYLAST, PM_REMOVE))
                DispatchMessageW(&message);
        }
        if (g_ime_result_count == before) {
            g_pending_high_surrogate = 0;
            committed_utf16_result_push(pending, count);
            g_ime_composing = FALSE;
            g_ime_ending++;
            imm_notify(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            g_ime_ending--;
        }
    } else if (count > 0) {
        g_ime_ending++;
        imm_notify(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        g_ime_ending--;
    }
    free(pending);
    imm_release_context(hwnd, context);
    if (g_ime_composing) composition_utf16_push(NULL, 0, 0);
}

/* Candidate window under the reported rectangle; for IMEs that still draw
 * their own composition window, start it at the same place. */
static void apply_ime_windows(HWND hwnd) {
    if (!hwnd || !g_caret_rect_valid) return;
    HIMC context = imm_get_context(hwnd);
    if (!context) return;
    COMPOSITIONFORM composition;
    memset(&composition, 0, sizeof(composition));
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos.x = g_caret_rect.left;
    composition.ptCurrentPos.y = g_caret_rect.top;
    CANDIDATEFORM candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos.x = g_caret_rect.left;
    candidate.ptCurrentPos.y = g_caret_rect.bottom;
    candidate.rcArea = g_caret_rect;
    if (g_imm_test) {
        g_imm_test->set_composition_window(context, &composition);
        g_imm_test->set_candidate_window(context, &candidate);
    } else {
        ImmSetCompositionWindow(context, &composition);
        ImmSetCandidateWindow(context, &candidate);
    }
    imm_release_context(hwnd, context);
}

/* SYS_TEXT_INPUT_CONTROL2(2, x | y << 32, width | height << 32): logical
 * units (points); the IME windows are placed in client pixels (× surface scale). */
i64 hmje_windows_text_input_caret(i64 position, i64 size) {
    RECT rect;
    LONG scale = (LONG)g_scale;
    rect.left = (LONG)((u64)position & 0x7fffffffull);
    rect.top = (LONG)(((u64)position >> 32) & 0x7fffffffull);
    rect.right = rect.left + (LONG)((u64)size & 0x7fffffffull);
    rect.bottom = rect.top + (LONG)(((u64)size >> 32) & 0x7fffffffull);
    if (GetEnvironmentVariableA("HMJE_WINDOWS_SELFTEST", NULL, 0) > 0 ||
        GetEnvironmentVariableA("HMJE_POSIX_SELFTEST_POINTER", NULL, 0) > 0) {
        static RECT last;
        if (!EqualRect(&rect, &last))
            fprintf(stderr, "windows-text-input: caret-rect %ld,%ld %ldx%ld\n", rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top);
        last = rect;
    }
    rect.left *= scale; rect.top *= scale; rect.right *= scale; rect.bottom *= scale;
    if (!g_caret_rect_valid || !EqualRect(&rect, &g_caret_rect)) {
        g_caret_rect = rect;
        g_caret_rect_valid = TRUE;
        apply_ime_windows(g_hwnd);
    }
    return 1;
}

i64 hmje_windows_text_input_control(i64 request) {
    u64 next = (u64)request;
    /* The safe-area bit is irrelevant for a desktop window and must not
     * cancel an in-progress composition on the same text client. */
    if (((next ^ g_text_input_request) & ~2ull) != 0) {
        g_pending_high_surrogate = 0;
        end_composition(g_hwnd, FALSE);
        if (g_hwnd && !g_imm_test) ImmAssociateContextEx(g_hwnd, NULL, (next & 1) ? IACE_DEFAULT : 0);
    }
    g_text_input_request = next;
    return 1;
}

static u64 legacy_pointer_event(u64 kind, u64 x, u64 y, u64 button, BOOL down) {
    u64 value = (kind << HMJE_INPUT_KIND_SHIFT)
              | (x & HMJE_INPUT_POINTER_COORD_MASK)
              | ((y & HMJE_INPUT_POINTER_COORD_MASK) << HMJE_INPUT_POINTER_Y_SHIFT)
              | ((button & 0x7full) << HMJE_INPUT_POINTER_BUTTON_SHIFT);
    if (down) value |= HMJE_INPUT_POINTER_PRESSED_FLAG;
    return value;
}

static void windows_pointer_push(
    u64 phase, u64 pointer_id, i64 x, i64 y, u64 buttons, u64 changed_button,
    u64 pressure, u64 altitude, u64 azimuth, i64 scroll_x, i64 scroll_y,
    u64 tool, u64 flags
) {
    hmje_pointer_record record;
    memset(&record, 0, sizeof(record));
    record.phase = phase;
    record.pointer_id = pointer_id;
    record.x = x;
    record.y = y;
    record.time_us = (u64)GetTickCount64() * 1000u;
    record.buttons = buttons;
    record.changed_button = changed_button;
    record.pressure = pressure;
    record.altitude = altitude;
    record.azimuth = azimuth;
    record.scroll_x = scroll_x;
    record.scroll_y = scroll_y;
    record.tool = tool;
    record.flags = flags;
    hmje_pointer_ring_push(&g_pointer_samples, record);
}

static u64 win_buttons(WPARAM wparam) {
    u64 buttons = 0;
    if (wparam & MK_LBUTTON) buttons |= 1u;
    if (wparam & MK_RBUTTON) buttons |= 2u;
    if (wparam & MK_MBUTTON) buttons |= 4u;
    return buttons;
}

static int is_promoted_pointer_message(void) {
    return ((u64)GetMessageExtraInfo() & 0xffffff00ull) == 0xff515700ull;
}

static void handle_mouse_pointer(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (is_promoted_pointer_message()) return;
    POINT point = {(LONG)(short)LOWORD(lparam), (LONG)(short)HIWORD(lparam)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        ScreenToClient(g_hwnd, &point);
    }
    i64 x = point.x < 0 ? 0 : point.x;
    i64 y = point.y < 0 ? 0 : point.y;
    u64 buttons = win_buttons(wparam);
    if (msg == WM_MOUSEMOVE) {
        u64 phase = buttons == 0 ? HMJE_POINTER_PHASE_HOVER : HMJE_POINTER_PHASE_MOVE;
        windows_pointer_push(phase, 1u, x, y, buttons, 0u, 0u, 0u, 0u, 0, 0,
                             HMJE_POINTER_TOOL_MOUSE, HMJE_POINTER_CAP_BUTTONS);
        legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, (u64)x, (u64)y, 0u, FALSE));
        return;
    }
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        i64 raw = (i64)GET_WHEEL_DELTA_WPARAM(wparam);
        /* Precision touchpads (and Windows' own touchpad inertia) send wheel deltas
         * smaller than a notch. Dividing by WHEEL_DELTA dropped them all, so touchpad
         * scrolling did nothing. Such deltas take the precise path of macOS
         * trackpads: points (one notch = 48 pt, HmjeUI's wheel step) accumulated
         * with a residual, published only as POLL2 scroll samples. */
        static double precise_residual_x = 0.0, precise_residual_y = 0.0;
        static BOOL precise_active = FALSE;
        if (raw % WHEEL_DELTA != 0 || precise_active) {
            precise_active = raw % WHEEL_DELTA != 0;
            double *residual = msg == WM_MOUSEHWHEEL ? &precise_residual_x : &precise_residual_y;
            /* AppKit signs (the contract): positive = see up/left. WM_MOUSEHWHEEL is
             * positive to the right, so the horizontal axis flips. */
            *residual += (msg == WM_MOUSEHWHEEL ? -1.0 : 1.0) * (double)raw * 48.0 / (double)WHEEL_DELTA;
            i64 points = *residual >= 0.0 ? (i64)floor(*residual) : (i64)ceil(*residual);
            *residual -= (double)points;
            if (points == 0) return;
            windows_pointer_push(HMJE_POINTER_PHASE_SCROLL, 1u, x, y, buttons, 0u, 0u, 0u, 0u,
                                 msg == WM_MOUSEHWHEEL ? points : 0, msg == WM_MOUSEWHEEL ? points : 0,
                                 HMJE_POINTER_TOOL_MOUSE, HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL);
            return;
        }
        i64 delta = raw / WHEEL_DELTA;
        if (msg == WM_MOUSEHWHEEL) {
            /* Horizontal notches (tilt wheel, Shift+wheel in some drivers) have no
             * legacy event: POLL2 pixels, one notch = 48 pt, AppKit sign — as the
             * macOS host does for horizontal notches. */
            windows_pointer_push(HMJE_POINTER_PHASE_SCROLL, 1u, x, y, buttons, 0u,
                                 0u, 0u, 0u, -delta * 48, 0, HMJE_POINTER_TOOL_MOUSE,
                                 HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL);
            return;
        }
        /* Vertical notches: the legacy notch event only, like the macOS host (a
         * POLL2 sample would add its notch count as points on top). */
        if (delta == 0) return;
        legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, (u64)x, (u64)y, 0u, FALSE));
        u64 amount = (u64)(delta < 0 ? -delta : delta);
        u64 packed = (HMJE_INPUT_KIND_POINTER_SCROLL << HMJE_INPUT_KIND_SHIFT)
                   | (amount & HMJE_INPUT_POINTER_COORD_MASK);
        if (delta < 0) packed |= HMJE_INPUT_SCROLL_DOWN_FLAG;
        legacy_input_push(packed);
        return;
    }
    BOOL down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
    /* The syllable belongs to the caret the click leaves: commit it before
     * the press is queued (HMJE moves the caret on the press). */
    if (down) { end_composition(g_hwnd, TRUE); SetFocus(g_hwnd); SetCapture(g_hwnd); }
    else if (buttons == 0 && GetCapture() == g_hwnd) ReleaseCapture();
    u64 button = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1u
               : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2u : 0u;
    windows_pointer_push(down ? HMJE_POINTER_PHASE_DOWN : HMJE_POINTER_PHASE_UP,
                         1u, x, y, buttons, button, 0u, 0u, 0u, 0, 0,
                         HMJE_POINTER_TOOL_MOUSE, HMJE_POINTER_CAP_BUTTONS);
    legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, (u64)x, (u64)y, 0u, FALSE));
    legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_BUTTON, (u64)x, (u64)y, button, down));
}

static void handle_native_pointer(UINT msg, WPARAM wparam) {
    UINT32 pointer_id = GET_POINTERID_WPARAM(wparam);
    POINTER_INFO info;
    if (!GetPointerInfo(pointer_id, &info)) return;
    POINT point = info.ptPixelLocation;
    ScreenToClient(g_hwnd, &point);
    u64 phase = msg == WM_POINTERDOWN ? HMJE_POINTER_PHASE_DOWN
              : msg == WM_POINTERUP ? HMJE_POINTER_PHASE_UP
              : (info.pointerFlags & POINTER_FLAG_INCONTACT) ? HMJE_POINTER_PHASE_MOVE
              : HMJE_POINTER_PHASE_HOVER;
    u64 buttons = (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) ? 1u : 0u;
    u64 tool = HMJE_POINTER_TOOL_UNKNOWN;
    u64 pressure = 0, altitude = 0, azimuth = 0;
    u64 flags = HMJE_POINTER_CAP_BUTTONS;
    if (info.pointerType == PT_PEN) {
        POINTER_PEN_INFO pen;
        tool = HMJE_POINTER_TOOL_PEN;
        if (GetPointerPenInfo(pointer_id, &pen)) {
            if (pen.penFlags & PEN_FLAG_ERASER) tool = HMJE_POINTER_TOOL_ERASER;
            pressure = ((u64)pen.pressure * 65535u) / 1024u;
            altitude = ((u64)(90 - min(90, abs(pen.tiltY))) * 65535u) / 90u;
            azimuth = ((u64)((pen.rotation % 360 + 360) % 360) * 65535u) / 360u;
            flags |= HMJE_POINTER_CAP_PRESSURE | HMJE_POINTER_CAP_ALTITUDE | HMJE_POINTER_CAP_AZIMUTH;
        }
    } else if (info.pointerType == PT_TOUCH) {
        tool = HMJE_POINTER_TOOL_TOUCH;
    } else if (info.pointerType == PT_MOUSE) {
        tool = HMJE_POINTER_TOOL_MOUSE;
    }
    if (phase == HMJE_POINTER_PHASE_DOWN) end_composition(g_hwnd, TRUE);
    windows_pointer_push(phase, pointer_id, point.x, point.y, buttons, 0u,
                         pressure, altitude, azimuth, 0, 0, tool, flags);
    u64 x = point.x < 0 ? 0u : (u64)point.x;
    u64 y = point.y < 0 ? 0u : (u64)point.y;
    legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, x, y, 0u, FALSE));
    if (phase == HMJE_POINTER_PHASE_DOWN || phase == HMJE_POINTER_PHASE_UP) {
        legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_BUTTON, x, y, 0u,
                                               phase == HMJE_POINTER_PHASE_DOWN));
    }
}

static LRESULT window_proc_routed(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* 메시지의 HWND로 창을 찾아 그 창 상태로 처리한다. 모르는 HWND(생성 중, 단위 시험의
 * 가짜 HWND)는 지금 창 그대로다. */
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        if (create != NULL && create->lpCreateParams != NULL) {
            hmje_windows_window *created = (hmje_windows_window *)create->lpCreateParams;
            created->hwnd = hwnd;
        }
    }
    hmje_windows_window *window = hmje_window_for_hwnd(hwnd);
    hmje_windows_window *previous = g_win;
    if (window != NULL) g_win = window;
    LRESULT result = window_proc_routed(hwnd, msg, wparam, lparam);
    g_win = previous;
    return result;
}

static LRESULT window_proc_routed(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_GETMINMAXINFO: {
        RECT minimum = {0, 0, (LONG)(MIN_CLIENT_WIDTH * g_scale), (LONG)(MIN_CLIENT_HEIGHT * g_scale)};
        AdjustWindowRect(&minimum, WS_OVERLAPPEDWINDOW, FALSE);
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = minimum.right - minimum.left;
        limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_SIZE:
        /* D3D can itself dispatch messages. Coalesce here; recreate resources
         * outside window_proc after the message queue has drained. A restored
         * window can have exactly the old size but still needs a repaint. */
        if (wparam != SIZE_MINIMIZED) {
            g_resize_pending = TRUE;
            if (g_has_presented_frame) g_repaint_pending = TRUE;
            if (g_win && g_win->id) {
                RECT frame; if (GetWindowRect(hwnd, &frame))
                    hmje_persist_frame(g_win->id, frame.left, frame.top,
                                       frame.right - frame.left, frame.bottom - frame.top);
            }
        }
        return 0;
    case WM_MOVE:
        if (g_win && g_win->id) {
            RECT frame; if (GetWindowRect(hwnd, &frame))
                hmje_persist_frame(g_win->id, frame.left, frame.top,
                                   frame.right - frame.left, frame.bottom - frame.top);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_DPICHANGED: {
        /* Moved to a monitor with another DPI: take the suggested rectangle; the
         * surface is recreated and the next resize event carries the new scale. */
        u64 scale = hmje_scale_for_dpi(HIWORD(wparam));
        RECT suggested = *(const RECT *)lparam;
        hmje_constrain_window_frame(&suggested);
        if (scale != g_scale) {
            HMJE_WIN->scale = scale;
            g_acked_width = 0;
        }
        SetWindowPos(hwnd, NULL, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_resize_pending = TRUE;
        return 0;
    }
    case WM_ERASEBKGND:
        if (g_has_presented_frame) return 1;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        if (g_has_presented_frame) g_repaint_pending = TRUE;
        return 0;
    }
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (wparam == VK_RETURN && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
            (g_ime_composing || g_ime_commit_return)) {
            g_ime_commit_return = FALSE;
            legacy_input_push(windows_key_event(wparam, lparam, TRUE) | (1ULL << 21));
            return 0;
        }
        if (wparam == VK_RETURN && (msg == WM_KEYUP || msg == WM_SYSKEYUP))
            g_ime_commit_return = FALSE;
        if (msg == WM_KEYDOWN && wparam == VK_PROCESSKEY) {
            /* The IME took this key (a jamo, Backspace inside the syllable).
             * Its key-up arrives with the real virtual key; match it by scan
             * code (ImmGetVirtualKey is already reset by the Korean IME). */
            u64 scan = ((u64)lparam >> 16) & 255;
            if (scan == 0) scan = MapVirtualKeyW(imm_get_virtual_key(hwnd), MAPVK_VK_TO_VSC) & 255;
            if (scan == 0x1c) g_ime_commit_return = TRUE;
            g_ime_keyups[scan / 64] |= 1ull << (scan % 64);
            return 0;
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            u64 scan = ((u64)lparam >> 16) & 255;
            if (g_ime_keyups[scan / 64] & (1ull << (scan % 64))) {
                g_ime_keyups[scan / 64] &= ~(1ull << (scan % 64));
                return 0;
            }
        }
        if (handle_windows_shortcut(hwnd, msg, wparam, lparam)) return 0;
        legacy_input_push(windows_key_event(wparam, lparam,
                          msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN));
        /* Preserve Windows system shortcuts such as Alt+F4. */
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        return 0;
    case WM_CHAR:
        committed_utf16_push((WCHAR)wparam);
        return 0;
    case WM_IME_SETCONTEXT:
        /* HMJE draws the composition; keep the IME's candidate UI only. */
        return DefWindowProcW(hwnd, msg, wparam, lparam & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
    case WM_IME_STARTCOMPOSITION:
        apply_ime_windows(hwnd);
        return 0;
    case WM_IME_COMPOSITION:
        if (g_ime_ending) return 0;
        /* Result first: a message can commit one syllable and start the next. */
        if ((lparam & GCS_RESULTSTR) && !committed_ime_result_push(hwnd)) {
            /* Unreadable result: let the default path deliver it as WM_CHAR. */
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        if (lparam & GCS_COMPSTR) composition_update_push(hwnd, lparam);
        return 0;
    case WM_IME_ENDCOMPOSITION:
        if (!g_ime_ending && g_ime_composing) composition_utf16_push(NULL, 0, 0);
        return 0;
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR) return TRUE;
        g_pending_high_surrogate = 0;
        committed_scalar_push((u64)wparam);
        return 0;
    case WM_KILLFOCUS:
        g_pending_high_surrogate = 0;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        handle_mouse_pointer(msg, wparam, lparam);
        return 0;
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
        handle_native_pointer(msg, wparam);
        return 0;
    case WM_CLOSE:
        /* 닫기 단추·Alt+F4. 창 사건을 아는 앱은 닫기 요청을 받아 거부할 수 있고,
         * 창은 앱이 루프를 끝낼 때(UI_WINDOW_OP_FINISH) 부순다. */
        if (g_window_events_aware && hmje_window_for_hwnd(hwnd) != NULL) {
            if (!HMJE_WIN->closing) window_push_event(HMJE_WIN, HMJE_WINDOW_EVENT_CLOSE_REQUEST);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_ACTIVATE:
        /* A selftest window stays key, like the macOS host's makeKeyAndOrderFront: a
         * window started from a service session (prlctl, CI) is often refused the
         * foreground, and an app that is told it resigned key hides its caret and
         * treats the next click as an activation click. */
        if (g_selftest_active && LOWORD(wparam) == WA_INACTIVE)
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        if (hmje_window_for_hwnd(hwnd) != NULL)
            HMJE_WIN->key_active = LOWORD(wparam) != WA_INACTIVE;
        if (g_window_events_aware && hmje_window_for_hwnd(hwnd) != NULL && !HMJE_WIN->closing)
            window_push_event(HMJE_WIN, LOWORD(wparam) == WA_INACTIVE
                ? HMJE_WINDOW_EVENT_RESIGNED_KEY : HMJE_WINDOW_EVENT_BECAME_KEY);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_DESTROY:
        g_closed = TRUE;
        if (g_window_events_aware) return 0;
        legacy_input_push(HMJE_INPUT_KIND_APP_CLOSE << HMJE_INPUT_KIND_SHIFT);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

static BOOL init_d3d11(void) {
    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.BufferCount = 1;
    desc.BufferDesc.Width = (UINT)g_width;
    desc.BufferDesc.Height = (UINT)g_height;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = g_hwnd;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    /* This presenter is event-driven, including idle restore paints. Retain
     * the committed backbuffer across presentation instead of DISCARD's
     * undefined contents when DWM re-exposes the restored window. */
    desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL obtained_level;

    /* 두 번째 창부터는 같은 장치에서 스왑체인만 새로 만든다(텍스처·셰이더·글리프
     * 캐시를 창들이 나눠 쓴다). */
    if (g_device != NULL) {
        IDXGIDevice *dxgi_device = NULL;
        IDXGIAdapter *adapter = NULL;
        IDXGIFactory *factory = NULL;
        HRESULT created = ID3D11Device_QueryInterface(g_device, &IID_IDXGIDevice, (void **)&dxgi_device);
        if (SUCCEEDED(created)) created = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
        if (SUCCEEDED(created)) created = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
        if (SUCCEEDED(created)) created = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)g_device, &desc, &g_swapchain);
        if (factory) IDXGIFactory_Release(factory);
        if (adapter) IDXGIAdapter_Release(adapter);
        if (dxgi_device) IDXGIDevice_Release(dxgi_device);
        if (FAILED(created)) {
            fprintf(stderr, "windows-surface: CreateSwapChain failed, hr=0x%08lx\n", (unsigned long)created);
            return FALSE;
        }
        goto buffers;
    }

    /* The existing presenter requires a hardware swapchain. CPU display-list
     * interpretation is separately and explicitly advertised by QUERY. */
    D3D_DRIVER_TYPE driver_types[] = {D3D_DRIVER_TYPE_HARDWARE};
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 1; i++) {
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, driver_types[i], NULL, 0,
            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
            &desc, &g_swapchain, &g_device, &obtained_level, &g_context);
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "windows-surface: D3D11 device created (driver_type=%d, feature_level=0x%x)\n",
                    (int)driver_types[i], (unsigned)obtained_level);
            break;
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "windows-surface: D3D11CreateDeviceAndSwapChain failed, hr=0x%08lx\n", (unsigned long)hr);
        return FALSE;
    }

buffers:
    hr = IDXGISwapChain_GetBuffer(g_swapchain, 0, &IID_ID3D11Texture2D, (void **)&g_backbuffer_tex);
    if (FAILED(hr)) {
        fprintf(stderr, "windows-surface: GetBuffer failed, hr=0x%08lx\n", (unsigned long)hr);
        return FALSE;
    }

    D3D11_TEXTURE2D_DESC staging_desc;
    memset(&staging_desc, 0, sizeof(staging_desc));
    staging_desc.Width = (UINT)g_width;
    staging_desc.Height = (UINT)g_height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    /* READ: self-test frame dumps read back the committed frame (both CPU and D3D paths). */
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(g_device, &staging_desc, NULL, &g_staging_tex);
    if (FAILED(hr)) {
        fprintf(stderr, "windows-surface: CreateTexture2D(staging) failed, hr=0x%08lx\n", (unsigned long)hr);
        return FALSE;
    }

    return TRUE;
}

/* Prepare replacement storage before touching the live surface. Copy committed
 * staging pixels separately from pending CPU blits, so resize/restore never
 * commits unfinished drawing. New space starts black, overlap keeps its pixels. */
static BOOL resize_surface(void) {
    if (!g_resize_pending || !g_ready || g_closed || IsIconic(g_hwnd)) return TRUE;
    RECT client;
    if (!GetClientRect(g_hwnd, &client)) return FALSE;
    LONG width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return TRUE;
    if ((u64)width == g_width && (u64)height == g_height && g_backbuffer_tex) {
        g_resize_pending = FALSE;
        return TRUE;
    }
    if (!g_device || !g_context || !g_swapchain || !g_staging_tex ||
        (u64)width > SIZE_MAX / 4 || (u64)height > SIZE_MAX / ((u64)width * 4)) return FALSE;
    hmje_d3d_resize_invalidate();
    u64 pitch = (u64)width * 4;
    unsigned char *pixels = (unsigned char *)calloc((size_t)height, (size_t)pitch);
    if (!pixels) return FALSE;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC description;
    ID3D11Texture2D_GetDesc(g_staging_tex, &description);
    description.Width = (UINT)width;
    description.Height = (UINT)height;
    D3D11_SUBRESOURCE_DATA initial = {pixels, (UINT)pitch, 0};
    HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &description, &initial, &staging);
    if (FAILED(hr)) goto failed;
    u64 copy_width = min(g_width, (u64)width), copy_height = min(g_height, (u64)height);
    for (u64 row = 0; row < copy_height; ++row)
        memcpy(pixels + row * pitch, g_backbuffer + row * g_pitch, (size_t)copy_width * 4);
    if (g_has_presented_frame) {
        D3D11_BOX overlap = {0, 0, 0, (UINT)copy_width, (UINT)copy_height, 1};
        ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)staging,
            0, 0, 0, 0, (ID3D11Resource *)g_staging_tex, 0, &overlap);
    }
    /* ResizeBuffers requires all old swapchain-buffer references/bindings to
     * be released. STAGING textures are independent resources. */
    ID3D11DeviceContext_ClearState(g_context);
    if (g_backbuffer_tex) ID3D11Texture2D_Release(g_backbuffer_tex);
    g_backbuffer_tex = NULL;
    hr = IDXGISwapChain_ResizeBuffers(g_swapchain, 0, (UINT)width, (UINT)height,
                                     DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        (void)IDXGISwapChain_GetBuffer(g_swapchain, 0, &IID_ID3D11Texture2D,
                                       (void **)&g_backbuffer_tex);
        goto failed;
    }
    hr = IDXGISwapChain_GetBuffer(g_swapchain, 0, &IID_ID3D11Texture2D,
                                  (void **)&g_backbuffer_tex);
    if (FAILED(hr)) goto failed;
    ID3D11Texture2D_Release(g_staging_tex);
    free(g_backbuffer);
    g_staging_tex = staging;
    g_backbuffer = pixels;
    g_width = (u64)width;
    g_height = (u64)height;
    g_pitch = pitch;
    g_resize_pending = FALSE;
    if (g_has_presented_frame) g_repaint_pending = TRUE;
    return TRUE;
failed:
    fprintf(stderr, "windows-surface: resize %ldx%ld failed, hr=0x%08lx\n",
            width, height, (unsigned long)hr);
    if (staging) ID3D11Texture2D_Release(staging);
    free(pixels);
    /* Preserve published geometry and retry on the next pump. If GetBuffer
     * failed after ResizeBuffers, no mismatched texture can be presented. */
    return FALSE;
}

static i64 surface_resize_event_if_needed(void) {
    if (!g_ready || g_closed || g_resize_pending || g_width == 0 || g_height == 0 ||
        (g_width == g_acked_width && g_height == g_acked_height)) return 0;
    g_acked_width = g_width;
    g_acked_height = g_height;
    return (i64)((5ull << HMJE_INPUT_KIND_SHIFT) | ((g_scale & 255ull) << 48) |
                 ((g_height & HMJE_INPUT_POINTER_COORD_MASK) << 24) |
                 (g_width & HMJE_INPUT_POINTER_COORD_MASK));
}

/* ---- HiDPI ------------------------------------------------------------------
 * 프로세스는 모니터별 DPI 인식(v2)이다: 창·표면·포인터 좌표는 물리 픽셀이고, HmjeUI는
 * 표면 배율(macOS Retina와 같은 정수 배율)로 포인트를 그린다. 배율 = DPI/96을 반올림
 * (100% → 1, 150%·200% → 2). HMJE_WINDOWS_SCALE=n은 배율을 고정하고, 1이면
 * DPI 비인식(시스템 확대)으로 둔다. */
static u64 g_forced_scale = 0;
static void hmje_dpi_init(void) {
    static BOOL initialized = FALSE;
    if (initialized) return;
    initialized = TRUE;
    char value[16];
    DWORD length = GetEnvironmentVariableA("HMJE_WINDOWS_SCALE", value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
        unsigned long long forced = strtoull(value, NULL, 10);
        if (forced >= 1 && forced <= 4) g_forced_scale = forced;
        if (forced == 1) return;
    }
    typedef BOOL (WINAPI *SetContext)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetContext set = user32 ? (SetContext)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
    if (set != NULL) (void)set((HANDLE)-4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
}

static u64 hmje_scale_for_dpi(UINT dpi) {
    if (g_forced_scale != 0) return g_forced_scale;
    u64 scale = ((u64)dpi + 48u) / 96u;
    return scale < 1 ? 1 : scale > 4 ? 4 : scale;
}

static UINT hmje_window_dpi(HWND hwnd) {
    typedef UINT (WINAPI *GetDpi)(HWND);
    typedef UINT (WINAPI *GetSystemDpi)(void);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) return 96;
    if (hwnd != NULL) {
        GetDpi get = (GetDpi)(void *)GetProcAddress(user32, "GetDpiForWindow");
        if (get != NULL) { UINT dpi = get(hwnd); if (dpi != 0) return dpi; }
    }
    GetSystemDpi system = (GetSystemDpi)(void *)GetProcAddress(user32, "GetDpiForSystem");
    UINT dpi = system != NULL ? system() : 96;
    return dpi != 0 ? dpi : 96;
}

static const wchar_t *hmje_window_class(void) {
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"HmjeWindowsHostWindowClass";
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    return L"HmjeWindowsHostWindowClass";
}

/* 창 하나를 만든다. 크기는 클라이언트 픽셀(Windows 표면 배율은 1). kind 2(보조)는 크기
 * 고정·가운데. 새 창(primary가 아닌)은 첫 프레임을 그린 뒤 보인다(show_window). */
static hmje_windows_window *create_window(u64 kind, u64 width, u64 height, BOOL positioned, i64 x, i64 y,
                                          const wchar_t *title, BOOL primary) {
    hmje_windows_window *window = (hmje_windows_window *)calloc(1, sizeof(*window));
    if (window == NULL) return NULL;
    window->id = g_next_window_id++;
    window->kind = kind == 0 ? 1 : kind;
    window->primary = primary;
    hmje_windows_window *previous = g_win;
    g_win = window;
    if (width == 0) width = DEFAULT_WIDTH;
    if (height == 0) height = DEFAULT_HEIGHT;
    hmje_dpi_init();
    window->scale = hmje_scale_for_dpi(hmje_window_dpi(NULL));
    width *= window->scale;
    height *= window->scale;
    RECT rect = {0, 0, (LONG)width, (LONG)height};
    DWORD style = window->kind == 2 ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) : WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);
    int outer_w = rect.right - rect.left, outer_h = rect.bottom - rect.top;
    int left = CW_USEDEFAULT, top = CW_USEDEFAULT;
    if (positioned) {
        left = (int)x;
        top = (int)y;
    } else if (window->kind == 2) {
        left = (GetSystemMetrics(SM_CXSCREEN) - outer_w) / 2;
        top = (GetSystemMetrics(SM_CYSCREEN) - outer_h) / 2;
    }
    /* Untitled windows show the program name (HelloUI.exe → "HelloUI"). */
    wchar_t program[MAX_PATH];
    if (title == NULL) {
        DWORD length = GetModuleFileNameW(NULL, program, MAX_PATH);
        const wchar_t *name = program;
        if (length > 0 && length < MAX_PATH) {
            for (const wchar_t *at = program; *at; ++at) if (*at == L'\\' || *at == L'/') name = at + 1;
            wchar_t *dot = wcsrchr((wchar_t *)name, L'.');
            if (dot != NULL) *dot = 0;
            title = name;
        }
    }
    CreateWindowExW(0, hmje_window_class(), title != NULL ? title : L"HMJE",
                    style, left, top, outer_w, outer_h, NULL, NULL, GetModuleHandleW(NULL), window);
    if (!g_hwnd) {
        g_win = previous;
        free(window);
        return NULL;
    }
    windows_drop_register(window);
    window->next = g_windows;
    g_windows = window;
    {
        /* The monitor the window landed on decides its scale (it may differ from the system DPI). */
        u64 actual = hmje_scale_for_dpi(hmje_window_dpi(g_hwnd));
        if (actual != window->scale) {
            RECT sized = {0, 0, (LONG)(width / window->scale * actual), (LONG)(height / window->scale * actual)};
            AdjustWindowRect(&sized, style, FALSE);
            SetWindowPos(g_hwnd, NULL, 0, 0, sized.right - sized.left, sized.bottom - sized.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            window->scale = actual;
        }
    }
    RECT initial_frame;
    if (GetWindowRect(g_hwnd, &initial_frame))
        hmje_restore_window_frame(g_hwnd, initial_frame.left, initial_frame.top,
                                  initial_frame.right - initial_frame.left, initial_frame.bottom - initial_frame.top);
    RECT client;
    GetClientRect(g_hwnd, &client);
    g_width = (u64)(client.right - client.left);
    g_height = (u64)(client.bottom - client.top);
    g_pitch = g_width * 4;
    g_backbuffer = (unsigned char *)calloc(1, (size_t)(g_pitch * g_height));

    fprintf(stderr, "windows-surface: window created %llux%llu\n",
            (unsigned long long)g_width, (unsigned long long)g_height);
    if (windows_debug_enabled())
        /* In points like the macOS host line (the surface itself is width x height pixels). */
        fprintf(stderr, "windows-window: open id=%llu kind=%llu %llux%llu\n", (unsigned long long)window->id,
                (unsigned long long)window->kind, (unsigned long long)(width / window->scale),
                (unsigned long long)(height / window->scale));

    if (!g_backbuffer || !init_d3d11()) {
        fprintf(stderr, "windows-surface: D3D11 init failed — framebuffer syscalls will report ENOSYS\n");
    }
    if (primary) {
        hmje_persist_record *saved = hmje_persist_bind(window->id, "__main__", 8, "", 0, 0);
        if (saved && saved->has_frame) {
            window->had_saved_frame = TRUE;
            hmje_restore_window_frame(g_hwnd, saved->x, saved->y, saved->width, saved->height);
        }
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
        window->shown = TRUE;
    }
    g_ready = TRUE;
    g_acked_width = g_width;
    g_acked_height = g_height;
    g_resize_pending = FALSE;
    g_win = previous;
    return window;
}

/* 마지막으로 앞에 나온 창 — 셀프테스트 동작의 대상. */
static hmje_windows_window *g_front_window = NULL;

static void show_window(hmje_windows_window *window) {
    if (window == NULL || window->shown || window->closing || window->hwnd == NULL) return;
    window->shown = TRUE;
    g_front_window = window;
    ShowWindow(window->hwnd, SW_SHOW);
    SetForegroundWindow(window->hwnd);
}

/* 첫 창(주 fiber) — 처음 표면을 쓸 때 만든다. */
static BOOL hmje_on_window_fiber(void);
static void hmje_fiber_main_bind(hmje_windows_window *window);
static void ensure_window(void) {
    if (g_win != NULL && g_win->ready) {
        return;
    }
    if (hmje_on_window_fiber()) return; /* 창 fiber의 창은 OPEN이 이미 만들었다. */
    hmje_windows_window *window = create_window(1, DEFAULT_WIDTH, DEFAULT_HEIGHT, FALSE, 0, 0, NULL, TRUE);
    if (window != NULL) {
        g_win = window;
        hmje_fiber_main_bind(window);
    }
}

static void pump_window_surface(void);

static void pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    /* 크기 바뀐 창의 표면 재생성과 복원 리페인트는 창마다 한다. */
    hmje_windows_window *previous = g_win;
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next) {
        g_win = window;
        pump_window_surface();
    }
    g_win = previous;
    if (g_windows == NULL) pump_window_surface();
}

static void pump_window_surface(void) {
    (void)resize_surface();
    /* Present outside window_proc to avoid re-entering the D3D presenter while
     * it dispatches Win32 messages. A restored idle app needs no new HMJE draw. */
    if (g_repaint_pending && g_has_presented_frame && !g_closed && !IsIconic(g_hwnd)) {
        g_repaint_pending = FALSE;
        /* Restore can report S_OK from Present before DWM has re-exposed its
         * surface. Wait for that compositor transition, then publish the
         * retained frame once; no periodic redraw or pending-blit upload. */
        HRESULT flush_result = DwmFlush();
        if (FAILED(flush_result))
            fprintf(stderr, "windows-surface: DwmFlush failed, hr=0x%08lx\n",
                    (unsigned long)flush_result);
        (void)present_staging_frame();
    }
}

/* ---- windows_shim.c 가 부르는 4개 함수 ------------------------------------------------ */

i64 hmje_windows_framebuffer_info(i64 out_address, i64 out_length) {
    if (out_address == 0 || out_length < 48) {
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    ensure_window();
    if (g_hwnd == NULL || g_backbuffer == NULL || g_device == NULL) {
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    u64 *out = (u64 *)(uintptr_t)(u64)out_address;
    out[0] = g_width;
    out[1] = g_height;
    out[2] = g_pitch;
    out[3] = 4; /* pixel_bytes */
    out[4] = 1; /* FRAMEBUFFER_FORMAT_XRGB8888 (canonical) */
    out[5] = 1; /* FRAMEBUFFER_FLAG_PRESENT */
    if (out_length >= 56) {
        out[6] = g_scale; /* v2 확장: 표면 배율(HiDPI 절 참고) */
        return 56;
    }
    return 48;
}

i64 hmje_windows_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y) {
    ensure_window();
    if (g_backbuffer == NULL || buffer_address == 0 || width <= 0 || height <= 0 || pitch <= 0) {
        return 0;
    }
    u64 uw = (u64)width, uh = (u64)height, upitch = (u64)pitch;
    if (uw > SIZE_MAX / 4 || upitch < uw * 4 || uh > SIZE_MAX / upitch) {
        return 0;
    }
    u64 ux = (u64)x, uy = (u64)y;
    if (ux >= g_width || uy >= g_height) {
        return 0;
    }
    u64 copy_w = uw;
    if (ux + copy_w > g_width) {
        copy_w = g_width - ux;
    }
    u64 copy_h = uh;
    if (uy + copy_h > g_height) {
        copy_h = g_height - uy;
    }
    if (copy_w == 0 || copy_h == 0) {
        return 0;
    }
    const unsigned char *src_base = (const unsigned char *)(uintptr_t)(u64)buffer_address;
    i64 total = 0;
    for (u64 row = 0; row < copy_h; row++) {
        const unsigned char *src_row = src_base + row * upitch;
        unsigned char *dst_row = g_backbuffer + (uy + row) * g_pitch + ux * 4;
        size_t bytes = (size_t)(copy_w * 4);
        memcpy(dst_row, src_row, bytes);
        total += (i64)bytes;
    }
    return total;
}

/* D3D11 present: CPU 백버퍼(malloc) → STAGING 텍스처(Map/memcpy) → CopyResource →
 * 스왑체인 백버퍼(DEFAULT) → Present(vsync). Metal의 "shared buffer → blit encoder →
 * drawable" 자리(파일 상단 주석 참고), 셰이더 없는 순수 업로드/블릿 단계. */
static void selftest_after_present(void);

static i64 present_staging_frame(void) {
    if (g_device == NULL || g_staging_tex == NULL || g_backbuffer_tex == NULL) {
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)g_backbuffer_tex,
                                      (ID3D11Resource *)g_staging_tex);
    HRESULT hr = IDXGISwapChain_Present(g_swapchain, 1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) g_repaint_pending = TRUE;
    if (FAILED(hr)) {
        fprintf(stderr, "windows-surface: Present failed, hr=0x%08lx\n", (unsigned long)hr);
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    selftest_after_present();
    return 1;
}

static i64 present_d3d11(void) {
    if (g_device == NULL || g_staging_tex == NULL || g_backbuffer_tex == NULL) {
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)g_staging_tex, 0,
                                          D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
    unsigned char *dst_base = (unsigned char *)mapped.pData;
    size_t row_bytes = (size_t)(g_width * 4);
    for (u64 row = 0; row < g_height; row++) {
        memcpy(dst_base + row * mapped.RowPitch, g_backbuffer + row * g_pitch, row_bytes);
    }
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)g_staging_tex, 0);

    g_repaint_pending = FALSE;
    i64 result = present_staging_frame();
    if (result > 0) {
        g_has_presented_frame = TRUE;
    }
    return result;
}

i64 hmje_windows_framebuffer_present(i64 rects_address, i64 count) {
    ensure_window();
    (void)rects_address;
    if (count <= 0 || g_closed) {
        return 0;
    }
    i64 result = present_d3d11();
    show_window(g_win);
    pump_messages();
    return result;
}

static void maybe_selftest(void);

i64 hmje_windows_input_poll(void) {
    ensure_window();
    pump_messages();
    i64 resize = surface_resize_event_if_needed();
    if (resize != 0) return resize;
    i64 event = legacy_input_pop();
    if (event == 0) {
        maybe_selftest();
        event = legacy_input_pop();
    }
    return event;
}

i64 hmje_windows_input_poll2(i64 out_address, i64 out_length) {
    ensure_window();
    pump_messages();
    return hmje_pointer_ring_pop(&g_pointer_samples, out_address, out_length);
}

#include "windows_d3d11_command_renderer.inc"

/* AppsUseLightTheme follows Windows Settings; dynamically load the registry API
 * so ordinary/headless target link recipes need no new DLL import library. */
i64 hmje_windows_system_theme(void) {
    typedef LSTATUS (WINAPI *GetValue)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    HMODULE module = LoadLibraryW(L"advapi32.dll");
    if (module == NULL) return 0;
    GetValue query = (GetValue)(void *)GetProcAddress(module, "RegGetValueW");
    DWORD light = 1, bytes = sizeof(light);
    LSTATUS status = query == NULL ? ERROR_PROC_NOT_FOUND : query(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &bytes);
    FreeLibrary(module);
    return status == ERROR_SUCCESS ? (light ? 1 : 2) : 0;
}

/* ---- 창 fiber ------------------------------------------------------------
 * macOS 호스트(posix_host_window.m)와 같은 모델: 창마다 HmjeUI 앱 루프가 하나씩
 * 돌고, 루프의 지역 상태를 창마다 따로 두려고 창마다 fiber를 쓴다. HWND 메시지는
 * 만든 스레드로 오므로 모든 창이 같은 HMJE 스레드의 fiber여야 한다. fiber는
 * UI_WINDOW_OP_WAIT(루프의 유휴 대기)·FINISH·RUN·OPEN/STARTED에서만 바뀐다. */
enum { HMJE_FIBER_NEW, HMJE_FIBER_RUNNING, HMJE_FIBER_WAITING, HMJE_FIBER_PARKED, HMJE_FIBER_DEAD };

typedef struct hmje_fiber {
    struct hmje_fiber *next;
    LPVOID handle;
    hmje_windows_window *win;
    void *(*entry)(void *);
    void *arg;
    int state;
    ULONGLONG deadline;
    BOOL is_main;
    struct hmje_fiber *opener;
} hmje_fiber;

static hmje_fiber g_fiber_main_storage;
static hmje_fiber *g_fiber_main = NULL;
static hmje_fiber *g_fiber_current = NULL;
static hmje_fiber *g_fibers = NULL;   /* 주 fiber를 뺀 살아 있는 fiber */
static hmje_fiber *g_fiber_zombies = NULL;

static void hmje_fibers_init(void) {
    if (g_fiber_main != NULL) return;
    g_fiber_main = &g_fiber_main_storage;
    g_fiber_main->is_main = TRUE;
    g_fiber_main->state = HMJE_FIBER_RUNNING;
    g_fiber_main->win = g_win; /* 이미 만든 첫 창(표시 함수 앱)은 주 fiber의 창이다 */
    g_fiber_main->handle = ConvertThreadToFiber(NULL);
    if (g_fiber_main->handle == NULL) g_fiber_main->handle = GetCurrentFiber();
    g_fiber_current = g_fiber_main;
}

static void hmje_fiber_main_bind(hmje_windows_window *window) {
    if (g_fiber_main != NULL) g_fiber_main->win = window;
}

static BOOL hmje_on_window_fiber(void) {
    return g_fiber_current != NULL && g_fiber_current != g_fiber_main;
}

static u64 hmje_live_window_count(void) {
    u64 count = 0;
    for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
        if (!window->closing) count++;
    return count;
}

static BOOL hmje_window_has_input(hmje_windows_window *window) {
    if (window == NULL) return FALSE;
    if (window->legacy_input.count != 0) return TRUE;
    if (window->pointer_samples.head != window->pointer_samples.tail) return TRUE;
    if (window->ready && !window->resize_pending &&
        (window->width != window->acked_width || window->height != window->acked_height)) return TRUE;
    return FALSE;
}

static BOOL hmje_main_should_return(void) {
    if (hmje_live_window_count() > 0) return FALSE;
    return TRUE; /* Windows에는 Dock 다시 열기가 없다 — 창이 없으면 RUN이 끝난다. */
}

static BOOL hmje_fiber_ready(hmje_fiber *fiber, ULONGLONG now) {
    switch (fiber->state) {
    case HMJE_FIBER_NEW: return TRUE;
    case HMJE_FIBER_WAITING: return now >= fiber->deadline || hmje_window_has_input(fiber->win);
    case HMJE_FIBER_PARKED: return hmje_main_should_return();
    default: return FALSE;
    }
}

static void hmje_fiber_switch_to(hmje_fiber *next) {
    if (next == g_fiber_current) {
        next->state = HMJE_FIBER_RUNNING;
        return;
    }
    g_fiber_current = next;
    next->state = HMJE_FIBER_RUNNING;
    g_win = next->win;
    SwitchToFiber(next->handle);
}

static void hmje_fiber_reap(void) {
    hmje_fiber **link = &g_fiber_zombies;
    while (*link != NULL) {
        hmje_fiber *fiber = *link;
        if (fiber == g_fiber_current) {
            link = &fiber->next;
            continue;
        }
        *link = fiber->next;
        DeleteFiber(fiber->handle);
        free(fiber);
    }
}

static hmje_fiber *hmje_fiber_pick(hmje_fiber *exclude, ULONGLONG now) {
    /* 지금 fiber 다음부터 돌아가며 찾는다(한 창이 독차지하지 않게). */
    hmje_fiber *order[64];
    size_t count = 0, start = 0;
    for (hmje_fiber *fiber = g_fibers; fiber != NULL && count < 63; fiber = fiber->next) order[count++] = fiber;
    order[count++] = g_fiber_main;
    for (size_t i = 0; i < count; i++) if (order[i] == g_fiber_current) start = i + 1;
    for (size_t step = 0; step < count; step++) {
        hmje_fiber *candidate = order[(start + step) % count];
        if (candidate != exclude && hmje_fiber_ready(candidate, now)) return candidate;
    }
    return NULL;
}

static ULONGLONG hmje_fiber_earliest_deadline(ULONGLONG fallback) {
    ULONGLONG earliest = fallback;
    for (hmje_fiber *fiber = g_fibers; fiber != NULL; fiber = fiber->next)
        if (fiber->state == HMJE_FIBER_WAITING && fiber->deadline < earliest) earliest = fiber->deadline;
    if (g_fiber_main->state == HMJE_FIBER_WAITING && g_fiber_main->deadline < earliest) earliest = g_fiber_main->deadline;
    return earliest;
}

static void hmje_fiber_block(int state, ULONGLONG deadline) {
    hmje_fiber *me = g_fiber_current;
    me->state = state;
    me->deadline = deadline;
    for (;;) {
        pump_messages();
        hmje_fiber_reap();
        ULONGLONG now = GetTickCount64();
        hmje_fiber *next = hmje_fiber_pick(me, now);
        if (next != NULL) {
            hmje_fiber_switch_to(next);
            if (me->state == HMJE_FIBER_RUNNING) return;
            continue;
        }
        if (state != HMJE_FIBER_DEAD && hmje_fiber_ready(me, now)) {
            me->state = HMJE_FIBER_RUNNING;
            return;
        }
        ULONGLONG until = hmje_fiber_earliest_deadline(now + 250);
        if (state == HMJE_FIBER_WAITING && deadline < until) until = deadline;
        DWORD wait = until > now ? (DWORD)(until - now) : 0;
        if (wait > 1000) wait = 1000;
        (void)MsgWaitForMultipleObjectsEx(0, NULL, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

extern i64 hmje_windows_ui_window(i64 operation, i64 argument1, i64 argument2);

static VOID CALLBACK hmje_fiber_entry(LPVOID parameter) {
    hmje_fiber *fiber = (hmje_fiber *)parameter;
    (void)fiber->entry(fiber->arg);
    (void)hmje_windows_ui_window(7, 0, 0); /* FINISH — 돌아오지 않는다 */
    abort();
}

static void destroy_window(hmje_windows_window *window) {
    if (window == NULL) return;
    hmje_windows_window *previous = g_win;
    g_win = window;
    window->closing = TRUE;
    hmje_d3d_resize_invalidate();
    for (int i = 0; i < 2; i++) hmje_d3d_surface_release(&g_d3d_target[i]);
    if (g_backbuffer_tex) ID3D11Texture2D_Release(g_backbuffer_tex);
    if (g_staging_tex) ID3D11Texture2D_Release(g_staging_tex);
    if (g_swapchain) IDXGISwapChain_Release(g_swapchain);
    g_backbuffer_tex = NULL;
    g_staging_tex = NULL;
    g_swapchain = NULL;
    if (window->drop_target) {
        RevokeDragDrop(window->hwnd);
        IDropTarget_Release(window->drop_target);
        window->drop_target = NULL;
    }
    if (window->drop_ole_initialized) OleUninitialize();
    if (window->hwnd) DestroyWindow(window->hwnd);
    while (window->drop_head) {
        HmjeDropRecord *next = window->drop_head->next;
        free(window->drop_head);
        window->drop_head = next;
    }
    free(window->backbuffer);
    free(window->pointer_samples.grown_records);
    free(window->legacy_input.grown_records);
    hmje_windows_window **link = &g_windows;
    while (*link != NULL && *link != window) link = &(*link)->next;
    if (*link == window) *link = window->next;
    if (g_front_window == window) g_front_window = NULL;
    if (windows_debug_enabled())
        fprintf(stderr, "windows-window: closed id=%llu (live %llu)\n", (unsigned long long)window->id,
                (unsigned long long)hmje_live_window_count());
    g_win = previous == window ? NULL : previous;
    free(window);
}

static i64 hmje_window_run_main(void) {
    hmje_fibers_init();
    if (g_fiber_current != g_fiber_main) return -22;
    for (;;) {
        hmje_fiber_block(HMJE_FIBER_PARKED, 0);
        if (hmje_live_window_count() == 0) {
            if (windows_debug_enabled()) fprintf(stderr, "windows-window: run -> quit\n");
            return 0;
        }
    }
}

static i64 hmje_window_finish(void) {
    hmje_fibers_init();
    hmje_fiber *me = g_fiber_current;
    hmje_windows_window *window = me->win != NULL ? me->win : g_win;
    if (window != NULL) destroy_window(window);
    me->win = NULL;
    g_win = NULL;
    if (!me->is_main) {
        if (me->opener != NULL) {
            me->opener->state = HMJE_FIBER_WAITING;
            me->opener->deadline = 0;
            me->opener = NULL;
        }
        hmje_fiber **link = &g_fibers;
        while (*link != NULL && *link != me) link = &(*link)->next;
        if (*link == me) *link = me->next;
        me->next = g_fiber_zombies;
        g_fiber_zombies = me;
        hmje_fiber_block(HMJE_FIBER_DEAD, 0);
        abort();
    }
    return hmje_window_run_main();
}

static wchar_t *hmje_wide_title(u64 address, u64 length) {
    if (address == 0 || length == 0 || length > (1u << 20)) return NULL;
    int wide = MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)address, (int)length, NULL, 0);
    if (wide <= 0) return NULL;
    wchar_t *text = (wchar_t *)calloc((size_t)wide + 1u, sizeof(wchar_t));
    if (text != NULL) MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)address, (int)length, text, wide);
    return text;
}

/* SYS_UI_WINDOW(117). 연산은 운영체제 계약.hmje UI_WINDOW_OP_*. */
i64 hmje_windows_ui_window(i64 operation, i64 argument1, i64 argument2) {
    switch (operation) {
    case 0: /* QUERY */
        if (((u64)argument1 & 1u) != 0) {
            g_window_events_aware = TRUE;
            hmje_fibers_init();
        }
        return 1 | (g_quit_requested ? 4 : 0); /* MULTIPLE (계속 실행 기본 아님) */
    case 1: { /* OPEN */
        if (argument1 == 0 || argument2 < 80) return -22;
        const u64 *descriptor = (const u64 *)(uintptr_t)(u64)argument1;
        if (descriptor[7] == 0) return -22;
        hmje_fibers_init();
        g_window_events_aware = TRUE;
        wchar_t *title = hmje_wide_title(descriptor[5], descriptor[6]);
        hmje_windows_window *window = create_window(descriptor[0], descriptor[1], descriptor[2],
                                                    (descriptor[9] & 1u) != 0, (i64)descriptor[3],
                                                    (i64)descriptor[4], title, FALSE);
        free(title);
        if (window == NULL) return -12;
        hmje_fiber *fiber = (hmje_fiber *)calloc(1, sizeof(*fiber));
        if (fiber == NULL) { destroy_window(window); return -12; }
        fiber->win = window;
        fiber->entry = (void *(*)(void *))(uintptr_t)descriptor[7];
        fiber->arg = (void *)(uintptr_t)descriptor[8];
        fiber->state = HMJE_FIBER_NEW;
        fiber->handle = CreateFiber((SIZE_T)64 << 20, hmje_fiber_entry, fiber);
        if (fiber->handle == NULL) { free(fiber); destroy_window(window); return -12; }
        fiber->next = g_fibers;
        g_fibers = fiber;
        /* 새 fiber로 곧장 넘어가 진입 값(훅)을 복사하게 한 뒤 STARTED로 돌아온다. */
        hmje_fiber *opener = g_fiber_current;
        fiber->opener = opener;
        opener->state = HMJE_FIBER_PARKED;
        hmje_fiber_switch_to(fiber);
        return (i64)window->id;
    }
    case 2: { /* CLOSE */
        hmje_windows_window *window = argument1 == 0 ? g_win : hmje_window_for_id((u64)argument1);
        if (window == NULL || window->hwnd == NULL) return -2;
        if (!window->closing) {
            RECT frame;
            if (GetWindowRect(window->hwnd, &frame))
                hmje_persist_frame(window->id, frame.left, frame.top,
                                   frame.right - frame.left, frame.bottom - frame.top);
            hmje_persist_close(window->id, g_quit_requested);
            window->closing = TRUE;
            ShowWindow(window->hwnd, SW_HIDE);
            window_push_event(window, HMJE_WINDOW_EVENT_CLOSE);
        }
        return 0;
    }
    case 3: /* CURRENT */
        return g_win != NULL ? (i64)g_win->id : 0;
    case 4: { /* POLICY — Windows에는 Dock이 없어 계속 실행이어도 창이 없으면 끝난다 */
        u64 previous = g_last_window_policy;
        if (argument1 == 1 || argument1 == 2) g_last_window_policy = (u64)argument1;
        return (i64)previous;
    }
    case 5: /* COUNT */
        return (i64)hmje_live_window_count();
    case 6: { /* ACTIVATE */
        hmje_windows_window *window = hmje_window_for_id((u64)argument1);
        if (window == NULL || window->closing) return -2;
        window->shown = TRUE;
        g_front_window = window;
        ShowWindow(window->hwnd, SW_SHOW);
        SetForegroundWindow(window->hwnd);
        return 0;
    }
    case 13: { /* SET_TITLE */
        hmje_windows_window *window = hmje_window_for_id((u64)argument1);
        if (window == NULL || window->hwnd == NULL || window->closing || argument2 == 0) return -22;
        const u64 *value = (const u64 *)(uintptr_t)(u64)argument2;
        if (value[1] > (1u << 20) || (value[1] && !value[0])) return -22;
        wchar_t *title = hmje_wide_title(value[0], value[1]);
        BOOL ok = hmje_set_window_title(window->hwnd, title != NULL ? title : L"");
        free(title);
        return ok ? 0 : -5;
    }
    case 14: { /* APP_STATE */
        if (hmje_live_window_count() == 0) return 3;
        BOOL visible = FALSE;
        for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
            if (window->hwnd != NULL && !window->closing && IsWindowVisible(window->hwnd) && !IsIconic(window->hwnd)) {
                visible = TRUE;
                break;
            }
        if (!visible) return 3;
        HWND foreground = GetForegroundWindow();
        for (hmje_windows_window *window = g_windows; window != NULL; window = window->next)
            if (window->hwnd == foreground && !window->closing) return 1;
        return 2;
    }
    case 15: { /* IS_OPEN */
        hmje_windows_window *window = hmje_window_for_id((u64)argument1);
        return window != NULL && window->hwnd != NULL && !window->closing ? 1 : 0;
    }
    case 21: { /* WINDOW_STATE */
        hmje_windows_window *window = hmje_window_for_id((u64)argument1);
        return window != NULL && window->hwnd != NULL && !window->closing &&
            (g_selftest_active ? g_front_window == window : window->key_active) ? 1 : 2;
    }
    case 16: { /* FIT_CONTENT */
        if (g_win == NULL || g_hwnd == NULL || argument1 == 0) return -22;
        const u64 *size = (const u64 *)(uintptr_t)(u64)argument1;
        if (size[0] > 2 || size[1] > 2) return -22;
        LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        if (size[0] == 2 || size[1] == 2) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        else style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, style);
        if (g_win->had_saved_frame && (size[0] == 2 || size[1] == 2)) return 0;
        RECT current = {0}; GetClientRect(g_hwnd, &current);
        int width = current.right, height = current.bottom;
        MONITORINFO monitor = {sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT work = monitor.rcWork;
        if (size[0] != 2 && size[2]) width = (int)(size[2] * g_win->scale);
        if (size[1] != 2 && size[3]) height = (int)(size[3] * g_win->scale);
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        RECT wanted = {0, 0, width, height}; AdjustWindowRect(&wanted, (DWORD)style, FALSE);
        int outer_w = wanted.right - wanted.left, outer_h = wanted.bottom - wanted.top;
        RECT old; GetWindowRect(g_hwnd, &old);
        int x = g_win->primary && !g_win->had_saved_frame ? work.left + (work.right - work.left - outer_w) / 2 : old.left;
        int y = g_win->primary && !g_win->had_saved_frame ? work.top + (work.bottom - work.top - outer_h) / 2 : old.top;
        RECT bounded = {x, y, x + outer_w, y + outer_h};
        hmje_constrain_window_frame(&bounded);
        SetWindowPos(g_hwnd, NULL, bounded.left, bounded.top, bounded.right - bounded.left,
                     bounded.bottom - bounded.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        hmje_persist_frame(g_win->id, bounded.left, bounded.top,
                           bounded.right - bounded.left, bounded.bottom - bounded.top);
        return 0;
    }
    case 17: { /* BIND */
        hmje_windows_window *window = hmje_window_for_id((u64)argument1);
        if (!window || !window->hwnd || !argument2) return -22;
        const u64 *data = (const u64 *)(uintptr_t)(u64)argument2;
        if (!data[0] || !data[1] || data[1] > HMJE_PERSIST_MAX_TEXT || data[3] > HMJE_PERSIST_MAX_TEXT ||
            (data[3] && !data[2])) return -22;
        hmje_persist_record *record = hmje_persist_bind(window->id, (const char *)(uintptr_t)data[0],
            (size_t)data[1], (const char *)(uintptr_t)data[2], (size_t)data[3], data[4] != 0);
        if (!record) return -12;
        if (record->has_frame) {
            window->had_saved_frame = TRUE;
            hmje_restore_window_frame(window->hwnd, record->x, record->y, record->width, record->height);
        }
        if (windows_debug_enabled())
            fprintf(stderr, "windows-window: frame id=%llu restored=%d %d,%d %dx%d\n",
                    (unsigned long long)window->id, window->had_saved_frame,
                    record->x, record->y, record->width, record->height);
        return 0;
    }
    case 18: { /* RESTORE */
        if (!argument1) return -22;
        const u64 *data = (const u64 *)(uintptr_t)(u64)argument1;
        if (!data[0] || !data[1] || data[1] > HMJE_PERSIST_MAX_TEXT || !data[3]) return -22;
        return hmje_persist_restore((const char *)(uintptr_t)data[0], (size_t)data[1],
            (uint32_t)data[2], (char *)(uintptr_t)data[3], (size_t)data[4]);
    }
    case 7: /* FINISH */
        return hmje_window_finish();
    case 8: /* RUN */
        g_window_events_aware = TRUE;
        return hmje_window_run_main();
    case 9: /* QUIT_CANCEL */
        g_quit_requested = FALSE;
        if (windows_debug_enabled()) fprintf(stderr, "windows-window: quit cancelled\n");
        return 0;
    case 10: /* QUIT */
        request_application_quit();
        return 0;
    case 11: { /* WAIT */
        hmje_fibers_init();
        if (argument1 < 0) return -22;
        hmje_fiber *me = g_fiber_current;
        hmje_fiber_block(HMJE_FIBER_WAITING, GetTickCount64() + (ULONGLONG)argument1);
        /* SYS_SLEEP_MS = Sleep 대기는 비동기 완료로 깨지 않는다.
         * 루프가 프레임마다 비동기 작업을 돌린다. 입력(bit 2)과 native 대기(bit 3)만 알린다. */
        return (i64)((hmje_window_has_input(me->win) ? 4 : 0) | 8);
    }
    case 12: { /* STARTED */
        hmje_fiber *me = g_fiber_current;
        hmje_fiber *opener = me != NULL ? me->opener : NULL;
        if (me == NULL || me->is_main || opener == NULL) return -22;
        me->opener = NULL;
        me->state = HMJE_FIBER_WAITING;
        me->deadline = 0;
        g_fiber_current = opener;
        opener->state = HMJE_FIBER_RUNNING;
        g_win = opener->win;
        SwitchToFiber(opener->handle);
        return 0;
    }
    default:
        return HMJE_WINDOWS_ENOSYS_LOCAL;
    }
}

#include "windows_selftest.inc"

/* 앱을 연 주소(SYS_APP_OPEN_URL)·URL 스킴 등록·단일 인스턴스 넘기기. */
#include "windows_open_url.inc"
