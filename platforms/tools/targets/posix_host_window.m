/*
 * posix_host_window.m — SYS_FRAMEBUFFER_* / SYS_INPUT_POLL을 실제 Cocoa 창으로 구현한다.
 * posix_host_shim.c(순수 C)에서 extern으로 호출하는 4개 함수만 외부에 노출한다.
 *
 * 계약은 x86 linear-fb /
 * aarch64 virtio-gpu 백엔드와 동일해야 한다:
 *   - blit(오프스크린 백버퍼 기록)과 present(commit)는 분리 — blit만으로는 화면에 안 보임.
 *   - 픽셀은 항상 canonical 0x00RRGGBB(리틀엔디안 자연수32 1픽셀).
 *
 * ── 표면 (GPU 렌더러) ─────────────────────────────
 * 표면은 CAMetalLayer다. CPU 백버퍼는 MTLBuffer(storageModeShared)의 contents를
 * 그대로 쓰고(blit syscall이 곧 GPU staging 버퍼에 기록 — 복사 0회), present마다
 * blit encoder로 drawable 텍스처(MTLPixelFormatBGRA8Unorm = canonical 바이트 배치
 * B,G,R,X와 정확히 일치, 스위즐 불필요)에 복사해 vsync(displaySync) 표시한다.

 *
 * 창은 리사이즈 가능하고 표면 크기는 "실제 창 크기 × 레티나 배율"의 픽셀 단위다.
 *   - SYS_FRAMEBUFFER_INFO는 v2 56바이트 레코드(오프셋 48 = 배율)를 반환한다.
 *   - 크기/배율이 바뀌면 백버퍼·drawable을 재구성하고 INPUT_EVENT_KIND_SURFACE_RESIZE
 *     (kind=5, 비트 0-23 너비, 24-47 높이, 48-55 배율)를 입력 링에 밀어넣는다.
 *     앱은 그 이벤트를 신호로 SYS_FRAMEBUFFER_INFO를 다시 읽고 전체를 다시 그린다.
 *   - Metal 디바이스가 없는 호스트(이례적)에서는 CGImage drawRect 경로로
 *     폴백한다 — 계약 시맨틱은 동일하고 vsync 보장만 없다.
 *
 * hmje 쪽 그리기 루프는 자체 imperative 루프(60Hz/잔여 sleep)를 돌리므로 [NSApp run]을
 * 절대 부르면 안 된다 — 그 대신 창을 lazily 만들고, present/input poll 때마다 Cocoa
 * run loop를 non-blocking으로 몇 차례 드레인해서 창이 "not responding"으로 안 보이게 하고
 * 이벤트를 흡수한다.
 *
 * 검증용 디버그 이스케이프 해치:
 *   - HMJE_POSIX_DUMP_FRAME=경로: 매 present마다 백버퍼(=drawable에 복사되는 것과 동일한
 *     픽셀)를 그 경로에 PNG로 덮어쓴다. 화면 캡처(TCC 권한)가 없는 세션에서 "창이 실제로
 *     뭘 그렸는지"를 검증한다. 정상 배포 시엔 미설정 → 오버헤드 없음.
 *   - HMJE_POSIX_SELFTEST_RESIZE=너비x높이[@프레임]: 지정한 present 횟수(기본 90) 뒤에
 *     창 콘텐츠 크기를 프로그램적으로 바꾼다 — 사람이 창 모서리를 끌지 않고도
 *     리사이즈 이벤트 → 앱 재배치 → 새 크기 PNG 덤프까지 자동 검증하기 위한 것.
 *     진행 로그는 stderr로만 낸다(stdout은 앱 출력 계약이라 오염 금지).
 *   - HMJE_POSIX_SELFTEST_LATENCY=1: 동작→present 지연, present(GPU 완료)·화면에 나온
 *     시각(presentedTime), nextDrawable 대기, 트랙패드 흐름 표본을 stderr에 남긴다(성능 벤치).
 *   - HMJE_POSIX_NO_VSYNC=1: 화면 주사 페이싱(hmje_vsync_*)을 끈다(비교 측정용).
 *   - HMJE_POSIX_DEBUG_VSYNC=1: 주사 틱마다 `posix-host-tick:`을 남긴다.
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

#include "pointer_input.h"

typedef long long i64;
typedef unsigned long long u64;
#include "window_persist.h"

/* 초기 창 크기(포인트). 이후에는 사용자가 자유롭게 리사이즈한다. */
#define DEFAULT_WIDTH 1180
#define DEFAULT_HEIGHT 860
#define MIN_CONTENT_WIDTH 320
#define MIN_CONTENT_HEIGHT 240

#define KEY_RING_CAPACITY 256

/* posix_host_shim.c의 HMJE_POSIX_ENOSYS와 값만 맞추면 되는 로컬 상수(별도 번역 단위). */
#define HMJE_POSIX_ENOSYS_LOCAL (-38LL)

/* library/kernel/api/syscall.hmje 계약과 값이 같아야 하는 로컬 상수(별도 번역 단위).
 * INPUT_EVENT_KIND_SURFACE_RESIZE: 비트 0-23 너비(픽셀), 24-47 높이(픽셀), 48-55 배율. */
#define HMJE_INPUT_KIND_SHIFT 56
#define HMJE_INPUT_KIND_SURFACE_RESIZE 5ULL
#define HMJE_INPUT_KIND_APP_CLOSE 6ULL
#define HMJE_INPUT_SURFACE_SIZE_MASK 16777215ULL
#define HMJE_INPUT_SURFACE_HEIGHT_SHIFT 24
#define HMJE_INPUT_SURFACE_SCALE_SHIFT 48
#define HMJE_INPUT_SURFACE_SCALE_MASK 255ULL

/* 포인터 이벤트. 커널(PS/2·virtio-input) 백엔드와 **같은 인코딩**이라 앱은 자기가
 * 호스트에 사는지 베어메탈에 사는지 모른다. 비트 0-23 x, 24-47 y, 48-54 버튼,
 * 55 눌림(버튼)/아래로(스크롤).
 *
 * 여기까지가 §25.3의 최소 네이티브 경계다 — Cocoa는 **이벤트 소스**일 뿐이고,
 * NSEvent에서 좌표와 버튼만 꺼내 우리 링에 넣는다. 히트테스트·클릭 판정·끌기
 * 임계는 전부 HmjeUI가 한다. 제스처도 같은 선을 긋는다: 원시 접점만 받고
 * 인식은 우리가 한다(핀치가 무엇인지가 플랫폼마다 갈리면 안 된다).
 */
#define HMJE_INPUT_KIND_POINTER_MOVE 1ULL
#define HMJE_INPUT_KIND_POINTER_BUTTON 2ULL
#define HMJE_INPUT_KIND_POINTER_SCROLL 3ULL
#define HMJE_INPUT_POINTER_COORD_MASK 16777215ULL
#define HMJE_INPUT_POINTER_Y_SHIFT 24
#define HMJE_INPUT_POINTER_BUTTON_SHIFT 48
#define HMJE_INPUT_POINTER_PRESSED_FLAG (1ULL << 55)
#define HMJE_INPUT_SCROLL_AMOUNT_MASK 16777215ULL
#define HMJE_INPUT_SCROLL_DOWN_FLAG (1ULL << 55)
/* 정밀 스크롤(트랙패드·매직마우스 표면)은 픽셀로 오므로 한 눈금 분량을 정해
 * 누적한다. 휠 마우스는 이미 눈금 단위라 그대로 쓴다. */
#define HMJE_PRECISE_SCROLL_STEP 10.0

#define KEY_RING_CAPACITY_WINDOW KEY_RING_CAPACITY
#define HMJE_GPU_LAYER_MAX 8
#define HMJE_AX_ACTION_CAPACITY 64

/* 창 생명주기(KIND_WINDOW, 운영체제 계약.hmje INPUT_WINDOW_EVENT_*). */
#define HMJE_INPUT_KIND_WINDOW 9ULL
#define HMJE_WINDOW_EVENT_CLOSE_REQUEST 1ULL
#define HMJE_WINDOW_EVENT_BECAME_KEY 2ULL
#define HMJE_WINDOW_EVENT_RESIGNED_KEY 3ULL
#define HMJE_WINDOW_EVENT_CLOSE 4ULL
#define HMJE_WINDOW_EVENT_QUIT_FLAG 256ULL

typedef struct { u64 identifier, action; } HmjeAXAction;
@class HmjeAccessibilityElement;

/* ---- 창 하나의 상태 ------------------------------------------------------
 * 창마다 한 객체로 상태를 묶고, 지금 syscall을 부른 창 흐름의 창을 g_win이 가리킨다.
 * `#define g_width (g_win->w_width)` 같은 별칭으로 현재 창의 상태를 읽고 쓴다.
 * AppKit 콜백(리사이즈·키·포인터·닫기)은 자기 창으로 g_win을 잠깐 바꿔(route)
 * 같은 코드를 탄다. 창이 없을 때는 모든 필드가 0/nil인 g_null_win을 가리킨다. */


@interface HmjeHostWindow : NSObject {
@public
    u64 w_id;
    u64 w_kind;
    NSWindow *w_window;
    NSView *w_view;
    hmje_pointer_ring w_pointer_samples;
    /* 표면 기하 — 전부 픽셀 단위(w_scale이 이미 곱해진 값). */
    u64 w_width;
    u64 w_height;
    u64 w_pitch;
    u64 w_scale;
    BOOL w_ready;
    BOOL w_closing;   /* 앱이 닫았다(UI_WINDOW_OP_CLOSE) — 루프가 끝나길 기다린다. */
    BOOL w_shown;     /* 새 창은 첫 프레임을 그린 뒤 앞으로 나온다. */
    BOOL w_key_active;
    BOOL w_primary;
    BOOL w_had_saved_frame;
    CAMetalLayer *w_layer;
    id<MTLBuffer> w_staging;
    id<MTLTexture> w_gpu_target;       /* target 0: 화면(캔버스값) */
    id<MTLTexture> w_gpu_target_bg;    /* target 1: 배경 */
    id<MTLTexture> w_gpu_scroll_scratch;
    BOOL w_gpu_target_needs_clear;
    BOOL w_gpu_target_bg_needs_clear;
    /* END_PRESENT는 GPU 완료를 기다리지 않는다. 완료 처리기가 실행 실패를 대상별
     * 비트(1=화면, 2=배경)로 남기고, 그 대상의 다음 BEGIN이 한 번 실패로 소비한다
     * (iOS 렌더러와 같은 계약 — 공통 렌더러가 동기화 플래그를 내리고 CPU로 복구). */
    atomic_uint w_gpu_async_failed;
    /* canonical 0x00RRGGBB, LE u32 per pixel. Metal 경로면 w_staging.contents,
     * 폴백(CGImage) 경로면 calloc 블록. */
    unsigned char *w_backbuffer;
    u64 w_key_ring[KEY_RING_CAPACITY_WINDOW];
    int w_key_ring_head; /* next slot to write */
    int w_key_ring_tail; /* next slot to read */
    u64 w_acked_width;
    u64 w_acked_height;
    u64 w_acked_scale;
    double w_precise_residual_x;
    double w_precise_residual_y;
    HmjeAXAction w_ax_actions[HMJE_AX_ACTION_CAPACITY];
    unsigned w_ax_read;
    unsigned w_ax_write;
    NSMutableDictionary<NSNumber *, HmjeAccessibilityElement *> *w_ax_elements;
    NSSet<NSNumber *> *w_ax_current;
    /* posix_host_text_input.m이 소유하는 창별 IME 상태(HmjeTextInputState). */
    id w_text_state;
    /* NSDraggingDestination events, consumed by the current window's UI fiber. */
    NSMutableArray<NSData *> *w_drop_events;
    /* 화면 주사(vsync) 페이싱 — hmje_vsync_* 주석. 틱은 이 창 뷰의 CADisplayLink가
     * 주사마다 올리고, present가 그 주사 번호를 적는다. */
    id w_display_link;
    u64 w_vsync_tick;
    u64 w_present_tick;
    BOOL w_vsync_running;
}
@end

@implementation HmjeHostWindow
@end

static HmjeHostWindow *g_null_win = nil;
static HmjeHostWindow *g_win = nil;
static u64 g_next_window_id = 1;
static NSMutableArray<HmjeHostWindow *> *g_windows = nil;
/* 마지막으로 앞(키)이 된 창 — 셀프테스트 동작의 대상. */
static __weak HmjeHostWindow *g_front_window = nil;

static HmjeHostWindow *hmje_null_window(void) {
    if (g_null_win == nil) {
        g_null_win = [[HmjeHostWindow alloc] init];
        g_null_win->w_scale = 1;
        g_null_win->w_gpu_target_needs_clear = YES;
        g_null_win->w_gpu_target_bg_needs_clear = YES;
    }
    return g_null_win;
}

#define HMJE_WIN (g_win != nil ? g_win : hmje_null_window())
#define g_window (HMJE_WIN->w_window)
#define g_view (HMJE_WIN->w_view)
#define g_pointer_samples (HMJE_WIN->w_pointer_samples)
#define g_width (HMJE_WIN->w_width)
#define g_height (HMJE_WIN->w_height)
#define g_pitch (HMJE_WIN->w_pitch)
#define g_scale (HMJE_WIN->w_scale)
#define g_ready (HMJE_WIN->w_ready)
#define g_layer (HMJE_WIN->w_layer)
#define g_staging (HMJE_WIN->w_staging)
#define g_gpu_target (HMJE_WIN->w_gpu_target)
#define g_gpu_target_bg (HMJE_WIN->w_gpu_target_bg)
#define g_gpu_scroll_scratch (HMJE_WIN->w_gpu_scroll_scratch)
#define g_gpu_target_needs_clear (HMJE_WIN->w_gpu_target_needs_clear)
#define g_gpu_target_bg_needs_clear (HMJE_WIN->w_gpu_target_bg_needs_clear)
#define g_backbuffer (HMJE_WIN->w_backbuffer)
#define g_key_ring (HMJE_WIN->w_key_ring)
#define g_key_ring_head (HMJE_WIN->w_key_ring_head)
#define g_key_ring_tail (HMJE_WIN->w_key_ring_tail)
#define g_acked_width (HMJE_WIN->w_acked_width)
#define g_acked_height (HMJE_WIN->w_acked_height)
#define g_acked_scale (HMJE_WIN->w_acked_scale)
#define g_ax_actions (HMJE_WIN->w_ax_actions)
#define g_ax_read (HMJE_WIN->w_ax_read)
#define g_ax_write (HMJE_WIN->w_ax_write)
#define g_ax_elements (HMJE_WIN->w_ax_elements)
#define g_ax_current (HMJE_WIN->w_ax_current)

static HmjeHostWindow *hmje_window_for_nswindow(NSWindow *window) {
    if (window == nil) return nil;
    for (HmjeHostWindow *candidate in g_windows) {
        if (candidate->w_window == window) return candidate;
    }
    return nil;
}

static HmjeHostWindow *hmje_window_for_id(u64 identifier) {
    for (HmjeHostWindow *candidate in g_windows) {
        if (candidate->w_id == identifier) return candidate;
    }
    return nil;
}

/* AppKit 콜백이 자기 창의 상태를 쓰도록 g_win을 잠깐 바꾼다. 돌려받은 값으로
 * 되돌린다(콜백은 hmje syscall 안의 이벤트 펌프에서 중첩될 수 있다). */
static HmjeHostWindow *hmje_route_begin(HmjeHostWindow *window) {
    HmjeHostWindow *previous = g_win;
    if (window != nil) g_win = window;
    return previous;
}

static void hmje_route_end(HmjeHostWindow *previous) {
    g_win = previous;
}

/* posix_host_text_input.m이 쓰는 창별 IME 상태와 경로 바꾸기. */
id hmje_posix_window_text_state(void) {
    return g_win != nil ? g_win->w_text_state : nil;
}

void hmje_posix_window_set_text_state(id state) {
    if (g_win != nil) g_win->w_text_state = state;
}

/* IME가 NSTextInputClient 메서드를 부를 때(키 이벤트 밖일 수도 있다) 그 뷰의
 * 창으로 큐를 돌린다. 돌려준 값을 hmje_posix_window_route_restore에 넘긴다. */
void *hmje_posix_window_route_nswindow(NSWindow *window) {
    HmjeHostWindow *host = hmje_window_for_nswindow(window);
    HmjeHostWindow *previous = hmje_route_begin(host);
    return (__bridge void *)previous;
}

void hmje_posix_window_route_restore(void *previous) {
    hmje_route_end((__bridge HmjeHostWindow *)previous);
}

static id g_application_delegate = nil;
static id g_key_monitor = nil;
static id g_pointer_monitor = nil;
static BOOL g_app_ready = NO;
static BOOL g_close_requested = NO;
static BOOL g_close_delivered = NO;
static BOOL g_runtime_polling = NO;
/* HmjeUI가 KIND_WINDOW 사건을 다룬다고 알렸다(UI_WINDOW_OP_QUERY a2 bit0).
 * 알리지 않은 앱은 닫기 = 앱 종료(APP_CLOSE)다. */
static BOOL g_window_events_aware = NO;
static BOOL g_quit_requested = NO;
static BOOL g_reopen_requested = NO;
/* 마지막 창이 닫힌 뒤의 정책(WINDOW_POLICY_*). macOS 기본은 계속 실행(메뉴 막대·Dock). */
static u64 g_last_window_policy = 2;

static BOOL windows_debug_enabled(void) {
    return getenv("HMJE_POSIX_DEBUG_WINDOWS") != NULL || getenv("HMJE_POSIX_SELFTEST_POINTER") != NULL;
}

static void window_ring_push(HmjeHostWindow *window, u64 event) {
    if (window == nil) return;
    int next = (window->w_key_ring_head + 1) % KEY_RING_CAPACITY_WINDOW;
    if (next == window->w_key_ring_tail) return;
    window->w_key_ring[window->w_key_ring_head] = event;
    window->w_key_ring_head = next;
}

static void window_push_event(HmjeHostWindow *window, u64 kind_event) {
    window_ring_push(window, (HMJE_INPUT_KIND_WINDOW << HMJE_INPUT_KIND_SHIFT) | kind_event);
    if (windows_debug_enabled() && window != nil)
        fprintf(stderr, "posix-host-window: event id=%llu kind=%llu\n", window->w_id, kind_event);
}

i64 hmje_posix_ui_window_config(i64 title_address, i64 title_length, i64 flags) {
    if (g_window == nil || title_length < 0 || (title_length > 0 && title_address == 0)) return HMJE_POSIX_ENOSYS_LOCAL;
    /* 화면 환경도 매 프레임 전달된다. 같은 native 속성은 다시 설정하지 않고,
     * AppKit이 반환하는 임시 객체는 이 OS 경계 안에서 회수한다. */
    @autoreleasepool {
        if (title_length > 0) {
            NSString *title = [[NSString alloc] initWithBytes:(const void *)(uintptr_t)(u64)title_address
                                                   length:(NSUInteger)title_length
                                                 encoding:NSUTF8StringEncoding];
            if (title == nil) return -22;
            if (![[g_window title] isEqualToString:title]) [g_window setTitle:title];
        }
        BOOL hidden = (((u64)flags & 1ULL) != 0);
        NSWindowTitleVisibility visibility = hidden ? NSWindowTitleHidden : NSWindowTitleVisible;
        if ([g_window titleVisibility] != visibility) [g_window setTitleVisibility:visibility];
        if ([g_window titlebarAppearsTransparent] != hidden) [g_window setTitlebarAppearsTransparent:hidden];
        NSUInteger previousStyle = [g_window styleMask];
        NSUInteger style = previousStyle;
        if (hidden) style |= NSWindowStyleMaskFullSizeContentView;
        else style &= ~NSWindowStyleMaskFullSizeContentView;
        if (style != previousStyle) [g_window setStyleMask:style];
        return 0;
    }
}

/* Metal 경로: 백버퍼는 shared MTLBuffer의 contents 그 자체(g_backbuffer가 가리킨다). */
static id<MTLDevice> g_device = nil;
static id<MTLCommandQueue> g_queue = nil;
/* 실제 앱의 명령 버퍼를 직접 그리는 hosted GPU 경로. 명령 해석은 hmje-ui의
 * gpu_command_renderer.hmje가 맡고, 이 상태는 정규화된 draw/blit 호출만 받는다.
 *
 * 대상(target) 두 개를 독립적으로 유지한다 — semtle_ui.hmje의 캔버스값(화면,
 * target=0, 매 프레임 실제 drawable에 present)과 배경(target=1, 앱이 직접
 * present하지 않는 persistent 콘텐츠 저장소, 캔버스값의 이미지 합성 소스)은
 * 서로 다른 텍스처가 필요하다 — 하나만 있으면 배경을 그 위에 그리는 순간
 * 캔버스값이 이미 present한 이전 프레임 내용을 덮어써 버린다.
 * BEGIN 요청의 세 번째 필드(target)가 어느 슬롯을 쓸지 고르고, 그 뒤 CLIP/
 * DRAW/UPLOAD_GLYPH/IMAGE_BLIT/SCROLL_SHIFT/END_PRESENT는 g_gpu_current_target로
 * 선택된 슬롯만 건드린다(hmje_gpu_active_target 참고). */
static int g_gpu_current_target = 0;
/* 세로 스크롤 shift·배경 합성 전용 임시 스크래치 텍스처 — 같은 텍스처 내
 * 겹치는 영역을 안전하게 옮기려고 경유한다(Metal에서 source/destination이
 * 같은 텍스처인 blit은 정의되지 않음). target 0/1 트랜잭션이 동시에 열리지
 * 않으므로(앱 프레임 루프가 배경을 완전히 끝내고서야 캔버스값을 시작한다)
 * 하나로 공유해도 안전하다. 필요한 크기가 커질 때만 다시 만들고 줄어들면 재사용한다. */
static id<MTLTexture> g_gpu_glyph = nil;
/* 그라데이션(GPU_COMMAND_GRADIENT_FILL) 단일 패스 파이프라인 — 블러처럼 지연
 * 생성해 기존 GPU 경로를 건드리지 않는다(스크래치 텍스처 불필요). */
static id<MTLRenderPipelineState> g_gpu_gradient_pipeline = nil;
static id<MTLRenderPipelineState> g_gpu_shape_pipeline = nil;
static id<MTLRenderPipelineState> g_gpu_glyph_pipeline = nil;
/* 이미지 스케일 블릿(GPU_COMMAND_IMAGE_SCALE_BLIT) 전용 텍스처-샘플 파이프라인.
 * 기존 g_gpu_shape_pipeline/g_gpu_glyph_pipeline과 같은 위치에서 지연 생성한다
 * (hmje_gpu_prepare_pipelines). 이미지 업로드는 명령별 불변 텍스처로 보존한다. */
static id<MTLRenderPipelineState> g_gpu_image_pipeline = nil;
/* 층(그룹 불투명도, HMJE_GPU_LAYER_BEGIN/END) — 이미지 파이프라인과 같은
 * 텍스처 쿼드지만 프래그먼트가 상수 알파를 실어 src-over로 얹는다. */
static id<MTLRenderPipelineState> g_gpu_layer_pipeline = nil;
/* 층 스택. 텍스처는 grow-only로 재사용한다 —
 * 한 프레임에 반투명 뷰가 수십 개여도 배열은 중첩 깊이까지만 자란다. */
static id<MTLTexture> g_gpu_layer_texture[HMJE_GPU_LAYER_MAX] = {nil};
static id<MTLTexture> g_gpu_layer_saved_target[HMJE_GPU_LAYER_MAX] = {nil};
static MTLScissorRect g_gpu_layer_saved_clip[HMJE_GPU_LAYER_MAX];
static u64 g_gpu_layer_x[HMJE_GPU_LAYER_MAX];
static u64 g_gpu_layer_y[HMJE_GPU_LAYER_MAX];
static u64 g_gpu_layer_w[HMJE_GPU_LAYER_MAX];
static u64 g_gpu_layer_h[HMJE_GPU_LAYER_MAX];
static u64 g_gpu_layer_alpha[HMJE_GPU_LAYER_MAX];
static int g_gpu_layer_depth = 0;
static id<MTLCommandBuffer> g_gpu_command_buffer = nil;
static id<MTLRenderCommandEncoder> g_gpu_render_encoder = nil;
static MTLScissorRect g_gpu_clip = {0, 0, 1, 1};

/* 현재 트랜잭션(BEGIN..END_PRESENT/ABORT)이 그리는 대상 텍스처. */
static id<MTLTexture> hmje_gpu_active_target(void) {
    if (g_gpu_layer_depth > 0) return g_gpu_layer_texture[g_gpu_layer_depth - 1];
    return g_gpu_current_target == 1 ? g_gpu_target_bg : g_gpu_target;
}
static BOOL *hmje_gpu_active_needs_clear_flag(void) {
    return g_gpu_current_target == 1 ? &g_gpu_target_bg_needs_clear : &g_gpu_target_needs_clear;
}
static void key_ring_push(u64 event) {
    /* full — drop oldest-arriving event rather than overwrite unread ones */
    window_ring_push(HMJE_WIN, event);
}

/* posix_host_text_input.m(키보드/IME 처리 유닛)가 쓰는 입력 링 push. */
void hmje_posix_input_push_event(u64 event) {
    key_ring_push(event);
}

/* posix_host_text_input.m가 구현한다 — 창 유닛은 이벤트를 넘기기만 한다. */
extern void hmje_posix_text_input_attach(NSWindow *window);
extern NSEvent *hmje_posix_text_input_handle_event(NSEvent *event);
extern void hmje_posix_text_input_selftest_tick(void);
extern void hmje_posix_text_input_pointer_down(void);
extern int hmje_posix_text_input_is_attached(void);
extern int hmje_posix_text_input_selftest_action(const char *action);

/* ---- 포인터 ------------------------------------------------------------- */
/* HmjeContentView는 isFlipped=YES이므로 convertPoint:fromView:nil의 원점은 좌상단이다.
 * Y를 다시 뒤집지 않고 배율을 곱해 표면 픽셀로 변환한다.
 * 창 밖 좌표는 가장자리로 제한하여 끌기 중 포인터가 튀지 않게 한다.
 * 합성 입력에도 동일한 역변환을 적용하면 좌표계 오류를 숨길 수 있다. */




static BOOL pointer_pixel_location(NSEvent *event, u64 *out_x, u64 *out_y) {
    if (g_view == nil || g_width == 0 || g_height == 0) {
        return NO;
    }
    NSPoint in_view = [g_view convertPoint:[event locationInWindow] fromView:nil];
    CGFloat scale_f = (CGFloat)g_scale;
    CGFloat px = in_view.x * scale_f;
    CGFloat py = in_view.y * scale_f;
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    u64 x = (u64)llround(px);
    u64 y = (u64)llround(py);
    if (x >= g_width) {
        x = g_width - 1;
    }
    if (y >= g_height) {
        y = g_height - 1;
    }
    *out_x = x;
    *out_y = y;
    return YES;
}

static u64 pointer_event_base(u64 kind, u64 x, u64 y) {
    return (kind << HMJE_INPUT_KIND_SHIFT)
         | (x & HMJE_INPUT_POINTER_COORD_MASK)
         | ((y & HMJE_INPUT_POINTER_COORD_MASK) << HMJE_INPUT_POINTER_Y_SHIFT);
}

static u64 pointer_time_us(NSEvent *event) {
    return (u64)(event.timestamp * 1000000.0);
}

static u64 pointer_buttons(NSEvent *event) {
    (void)event;
    return (u64)[NSEvent pressedMouseButtons];
}

/* NSEventSubtypeTabletPoint의 값(1)은 ScrollWheel의 continuous subtype 값과
 * 겹친다. 따라서 subtype만 보고 태블릿 접근자(pointingDeviceID/pressure/tilt)를
 * 호출하면 트랙패드 스크롤에서 AppKit이 NSInternalInconsistencyException을 던진다.
 * 태블릿 payload를 가질 수 있는 이벤트 종류를 먼저 제한한 뒤 subtype을 읽는다. */
static BOOL pointer_event_type_supports_tablet(NSEventType type) {
    switch (type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
        case NSEventTypeTabletPoint:
            return YES;
        default:
            return NO;
    }
}

static BOOL pointer_event_is_tablet(NSEvent *event) {
    return pointer_event_type_supports_tablet([event type])
        && [event subtype] == NSEventSubtypeTabletPoint;
}

/* 펜 종류(pointingDeviceType)와 펜 번호(pointingDeviceID)는 AppKit 계약상
 * **proximity** 이벤트에서만 유효하다(NSEvent.h). point 이벤트에서 읽으면 예외는
 * 안 나도 0이 돌아와 지우개가 영영 펜으로 보인다. proximity에서 deviceID로 묶어
 * 기억해 두고, point 이벤트는 둘 다에서 유효한 deviceID로 찾는다. */
#define POINTER_TABLET_DEVICE_CAP 8u
typedef struct pointer_tablet_device {
    NSUInteger device_id;
    NSUInteger pointing_device_id;
    BOOL eraser;
    BOOL used;
} pointer_tablet_device;
static pointer_tablet_device g_tablet_devices[POINTER_TABLET_DEVICE_CAP];

static BOOL pointer_event_is_tablet_proximity(NSEvent *event) {
    NSEventType type = [event type];
    if (type == NSEventTypeTabletProximity) {
        return YES;
    }
    return type != NSEventTypeTabletPoint
        && pointer_event_type_supports_tablet(type)
        && [event subtype] == NSEventSubtypeTabletProximity;
}

static pointer_tablet_device *pointer_tablet_device_find(NSUInteger device_id) {
    for (unsigned i = 0; i < POINTER_TABLET_DEVICE_CAP; i++) {
        if (g_tablet_devices[i].used && g_tablet_devices[i].device_id == device_id) {
            return &g_tablet_devices[i];
        }
    }
    return NULL;
}

static void pointer_tablet_proximity(NSEvent *event) {
    NSUInteger device_id = event.deviceID;
    pointer_tablet_device *device = pointer_tablet_device_find(device_id);
    if (!event.enteringProximity) {
        if (device != NULL) {
            device->used = NO;
        }
        return;
    }
    if (device == NULL) {
        for (unsigned i = 0; i < POINTER_TABLET_DEVICE_CAP && device == NULL; i++) {
            if (!g_tablet_devices[i].used) {
                device = &g_tablet_devices[i];
            }
        }
    }
    if (device == NULL) {
        device = &g_tablet_devices[0];  /* 가득 차면 가장 앞 칸을 재사용한다. */
    }
    device->used = YES;
    device->device_id = device_id;
    device->pointing_device_id = event.pointingDeviceID;
    device->eraser = event.pointingDeviceType == NSPointingDeviceTypeEraser;
}

static void pointer_push_sample(
    NSEvent *event, u64 phase, u64 changed_button, i64 scroll_x, i64 scroll_y
) {
    u64 x = 0, y = 0;
    if (!pointer_pixel_location(event, &x, &y)) {
        return;
    }
    hmje_pointer_record record;
    memset(&record, 0, sizeof(record));
    record.phase = phase;
    record.pointer_id = 1u;
    record.x = (i64)x;
    record.y = (i64)y;
    record.time_us = pointer_time_us(event);
    record.buttons = pointer_buttons(event);
    record.changed_button = changed_button;
    record.scroll_x = scroll_x;
    record.scroll_y = scroll_y;
    record.tool = HMJE_POINTER_TOOL_MOUSE;
    record.flags = HMJE_POINTER_CAP_BUTTONS;

    if (pointer_event_is_tablet(event)) {
        pointer_tablet_device *device = pointer_tablet_device_find(event.deviceID);
        record.pointer_id = device != NULL ? (u64)device->pointing_device_id : 0u;
        record.tool = device != NULL && device->eraser
            ? HMJE_POINTER_TOOL_ERASER : HMJE_POINTER_TOOL_PEN;
        double pressure = event.pressure;
        if (pressure < 0.0) pressure = 0.0;
        if (pressure > 1.0) pressure = 1.0;
        record.pressure = (u64)llround(pressure * 65535.0);
        record.flags |= HMJE_POINTER_CAP_PRESSURE;

        NSPoint tilt = event.tilt;
        double magnitude = hypot(tilt.x, tilt.y);
        if (magnitude > 1.0) magnitude = 1.0;
        record.altitude = (u64)llround((1.0 - magnitude) * 65535.0);
        double azimuth = atan2(tilt.y, tilt.x);
        if (azimuth < 0.0) azimuth += 2.0 * M_PI;
        record.azimuth = (u64)llround((azimuth / (2.0 * M_PI)) * 65535.0);
        record.flags |= HMJE_POINTER_CAP_ALTITUDE | HMJE_POINTER_CAP_AZIMUTH;
    }
    if (phase == HMJE_POINTER_PHASE_SCROLL) {
        record.flags |= HMJE_POINTER_CAP_SCROLL;
    }
    hmje_pointer_ring_push(&g_pointer_samples, record);
}

static void pointer_push_move(NSEvent *event) {
    u64 x = 0, y = 0;
    if (!pointer_pixel_location(event, &x, &y)) {
        return;
    }
    key_ring_push(pointer_event_base(HMJE_INPUT_KIND_POINTER_MOVE, x, y));
    u64 phase = event.type == NSEventTypeMouseMoved
        ? HMJE_POINTER_PHASE_HOVER : HMJE_POINTER_PHASE_MOVE;
    pointer_push_sample(event, phase, 0u, 0, 0);
}

/* 버튼 번호는 커널과 같다: 0 왼쪽, 1 오른쪽, 2 가운데. NSEvent.buttonNumber가
 * 마침 같은 순서라 변환이 없다. 그 위 버튼(4번째 이상)은 아직 계약에 없어 버린다. */
static void pointer_push_button(NSEvent *event, BOOL down) {
    u64 x = 0, y = 0;
    if (!pointer_pixel_location(event, &x, &y)) {
        return;
    }
    NSInteger button = [event buttonNumber];
    if (button < 0 || button > 2) {
        return;
    }
    /* 버튼 이벤트는 좌표도 실어 나르지만, 누른 자리로 포인터가 먼저 가 있어야
     * 히트테스트가 맞는다 — 이동을 한 번 앞세운다(커널 PS/2 경로도 패킷마다
     * 이동을 먼저 넣는다). */
    key_ring_push(pointer_event_base(HMJE_INPUT_KIND_POINTER_MOVE, x, y));
    u64 packed = pointer_event_base(HMJE_INPUT_KIND_POINTER_BUTTON, x, y)
               | (((u64)button) << HMJE_INPUT_POINTER_BUTTON_SHIFT);
    if (down) {
        packed |= HMJE_INPUT_POINTER_PRESSED_FLAG;
    }
    key_ring_push(packed);
    pointer_push_sample(
        event,
        down ? HMJE_POINTER_PHASE_DOWN : HMJE_POINTER_PHASE_UP,
        (u64)button,
        0,
        0
    );
}

/* 정밀 델타(포인트, 소수)를 창별 잔여에 더하고 정수 부분만 꺼낸다. 트랙패드·매직마우스
 * 표면과 셀프테스트 트랙패드 흐름(selftest_trackpad_deliver)이 같은 누적을 쓴다. */
static BOOL pointer_precise_scroll_take(double delta_x, double delta_y, i64 *out_x, i64 *out_y) {
#define precise_residual_x (HMJE_WIN->w_precise_residual_x)
#define precise_residual_y (HMJE_WIN->w_precise_residual_y)
    precise_residual_x += delta_x;
    precise_residual_y += delta_y;
    i64 pixel_x = precise_residual_x >= 0.0
        ? (i64)floor(precise_residual_x) : (i64)ceil(precise_residual_x);
    i64 pixel_y = precise_residual_y >= 0.0
        ? (i64)floor(precise_residual_y) : (i64)ceil(precise_residual_y);
    precise_residual_x -= (double)pixel_x;
    precise_residual_y -= (double)pixel_y;
#undef precise_residual_x
#undef precise_residual_y
    *out_x = pixel_x;
    *out_y = pixel_y;
    return pixel_x != 0 || pixel_y != 0;
}

/* 노치 휠(정밀 델타가 없는 마우스 휠) 한 사건. 세로 노치는 POLL1 노치 사건,
 * 가로 노치(Shift+휠 — AppKit이 세로 노치를 가로 델타로 바꿔 준다 — 와 가로 휠)는 POLL1
 * 계약에 가로가 없으므로 POLL2 정밀 표본의 가로 픽셀로 싣는다: 한 노치 = 48pt(HmjeUI 휠
 * 한 칸). 앱의 가로 스크롤 영역(코드 편집기의 긴 줄)이 가로 성분을 읽는다. AppKit 부호를
 * 그대로 쓴다(양수 = 내용이 오른쪽/아래로 = 왼쪽/위를 본다). */
static void pointer_push_notches(u64 x, u64 y, u64 time_us, long long across, long long notches) {
    if (across != 0) {
        hmje_pointer_record record;
        memset(&record, 0, sizeof(record));
        record.phase = HMJE_POINTER_PHASE_SCROLL;
        record.pointer_id = 1u;
        record.x = (i64)x;
        record.y = (i64)y;
        record.time_us = time_us;
        record.scroll_x = (i64)(across * 48);
        record.scroll_y = 0;
        record.tool = HMJE_POINTER_TOOL_MOUSE;
        record.flags = HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL;
        hmje_pointer_ring_push(&g_pointer_samples, record);
    }
    if (notches == 0) {
        return;
    }
    /* AppKit의 양수 deltaY는 "콘텐츠가 아래로" = 위로 스크롤이다. 계약의
     * DOWN 플래그는 그 반대쪽이므로 부호를 그대로 읽어 나눈다. */
    BOOL down = (notches < 0);
    u64 amount = (u64)(down ? -notches : notches);
    if (amount > HMJE_INPUT_SCROLL_AMOUNT_MASK) {
        amount = HMJE_INPUT_SCROLL_AMOUNT_MASK;
    }
    key_ring_push(pointer_event_base(HMJE_INPUT_KIND_POINTER_MOVE, x, y));
    u64 packed = (HMJE_INPUT_KIND_POINTER_SCROLL << HMJE_INPUT_KIND_SHIFT)
               | (amount & HMJE_INPUT_SCROLL_AMOUNT_MASK);
    if (down) {
        packed |= HMJE_INPUT_SCROLL_DOWN_FLAG;
    }
    key_ring_push(packed);
}

static void pointer_push_scroll(NSEvent *event) {
    u64 x = 0, y = 0;
    if (!pointer_pixel_location(event, &x, &y)) {
        return;
    }
    /* Modern trackpads already provide pixel deltas (including AppKit's
     * momentum stream).  Publish every one through POLL2 instead of waiting
     * until an arbitrary notch threshold is crossed.  The legacy packed
     * wheel event remains only for discrete mouse wheels / old guests. */
    if ([event hasPreciseScrollingDeltas]) {
        i64 pixel_x = 0, pixel_y = 0;
        if (!pointer_precise_scroll_take([event scrollingDeltaX], [event scrollingDeltaY], &pixel_x, &pixel_y)) return;
        pointer_push_sample(
            event,
            HMJE_POINTER_PHASE_SCROLL,
            0u,
            pixel_x,
            pixel_y
        );
        return;
    }
    pointer_push_notches(x, y, pointer_time_us(event),
                         (long long)llround([event scrollingDeltaX]),
                         (long long)llround([event scrollingDeltaY]));
}

/* 모니터 하나로 전부 받는다. 이벤트를 **소비하지 않고 그대로 돌려준다** —
 * 창 이동·리사이즈 같은 AppKit 자체 동작을 막으면 안 된다. */
static NSEvent *pointer_handle_event_routed(NSEvent *event);

static NSEvent *pointer_handle_event(NSEvent *event) {
    if (pointer_event_is_tablet_proximity(event)) {
        pointer_tablet_proximity(event);
    }
    /* 이벤트가 속한 창의 큐로 보낸다. 우리 창이 아니면(메뉴·패널) 건드리지 않는다. */
    HmjeHostWindow *window = hmje_window_for_nswindow([event window]);
    if (window == nil || window->w_closing) {
        return event;
    }
    HmjeHostWindow *previous = hmje_route_begin(window);
    NSEvent *result = pointer_handle_event_routed(event);
    hmje_route_end(previous);
    return result;
}

static NSEvent *pointer_handle_event_routed(NSEvent *event) {
    switch ([event type]) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            pointer_push_move(event);
            break;
        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown:
            /* 조합 중인 글자는 누르기 전 캐럿 자리의 것 — 포인터보다 먼저 확정한다. */
            hmje_posix_text_input_pointer_down();
            pointer_push_button(event, YES);
            break;
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp:
            pointer_push_button(event, NO);
            break;
        case NSEventTypeScrollWheel:
            pointer_push_scroll(event);
            break;
        default:
            break;
    }
    return event;
}

static i64 key_ring_pop(void) {
    if (g_key_ring_tail == g_key_ring_head) {
        return 0;
    }
    u64 value = g_key_ring[g_key_ring_tail];
    g_key_ring_tail = (g_key_ring_tail + 1) % KEY_RING_CAPACITY;
    return (i64)value;
}

static BOOL surface_debug_enabled(void) {
    return getenv("HMJE_POSIX_DEBUG_SURFACE") != NULL || getenv("HMJE_POSIX_SELFTEST_RESIZE") != NULL;
}

static void surface_debug_log(const char *tag) {
    if (!surface_debug_enabled()) {
        return;
    }
    fprintf(stderr, "posix-host-surface: %s %llux%llu scale=%llu\n", tag, g_width, g_height, g_scale);
}

/* ---- 표면(백버퍼 + drawable) 기하 갱신 ------------------------------------ */

/* 새 픽셀 크기로 백버퍼를 재할당한다. 이전 내용의 좌상단 교집합은 복사해
 * 리사이즈 순간의 깜빡임(검은 프레임)을 줄인다 — 앱이 곧 SURFACE_RESIZE 이벤트를
 * 받고 전체를 다시 그린다. */
static BOOL reallocate_backbuffer(u64 new_width, u64 new_height) {
    u64 new_pitch = new_width * 4;
    unsigned char *old_pixels = g_backbuffer;
    u64 old_width = g_width, old_height = g_height, old_pitch = g_pitch;
    /* 주의: 이전 백버퍼의 유일한 강한 참조가 g_staging이다. 보존 복사가 끝나기 전에
     * g_staging을 덮어쓰면 ARC가 이전 버퍼를 즉시 해제해 old_pixels가 dangling이 된다
     * (실제로 setContentSize 경로에서 memmove SIGSEGV로 재현). 복사가 끝날 때까지
     * 지역 참조로 살려둔다. 마지막 present는 waitUntilCompleted라 GPU 사용도 끝난 상태. */
    id<MTLBuffer> old_buffer = g_staging;
    id<MTLBuffer> new_buffer = nil;
    unsigned char *new_pixels = NULL;

    if (g_device != nil) {
        new_buffer = [g_device newBufferWithLength:(NSUInteger)(new_pitch * new_height)
                                           options:MTLResourceStorageModeShared];
        if (new_buffer == nil) {
            return NO;
        }
        new_pixels = (unsigned char *)[new_buffer contents];
        memset(new_pixels, 0, (size_t)(new_pitch * new_height));
    } else {
        new_pixels = calloc(1, (size_t)(new_pitch * new_height));
        if (new_pixels == NULL) {
            return NO;
        }
    }

    if (old_pixels != NULL) {
        u64 copy_w = old_width < new_width ? old_width : new_width;
        u64 copy_h = old_height < new_height ? old_height : new_height;
        for (u64 row = 0; row < copy_h; row++) {
            memcpy(new_pixels + row * new_pitch, old_pixels + row * old_pitch, (size_t)(copy_w * 4));
        }
        if (g_device == nil) {
            free(old_pixels);
        }
    }

    g_staging = new_buffer; /* 이제야 이전 버퍼 참조를 놓는다(old_buffer가 함수 끝까지 유지). */
    g_backbuffer = new_pixels;
    g_width = new_width;
    g_height = new_height;
    g_pitch = new_pitch;
    (void)old_buffer;
    return YES;
}

/* 뷰의 현재 포인트 크기 × 배율 → 픽셀 표면. 변화가 있을 때만 재구성하고
 * SURFACE_RESIZE 이벤트를 큐잉한다(초기 생성 시에는 push_event=NO). */
static void update_surface_geometry(BOOL push_event) {
    if (g_view == nil || g_window == nil) {
        return;
    }
    CGFloat scale_f = [g_window backingScaleFactor];
    if (scale_f < 1.0) {
        scale_f = 1.0;
    }
    /* HmjeContentView는 이 지점에서 아직 forward 선언뿐이라 NSView로 캐스팅해 보낸다. */
    NSSize bounds = [(NSView *)g_view bounds].size;
    u64 new_scale = (u64)llround(scale_f);
    if (new_scale < 1) {
        new_scale = 1;
    }
    u64 new_width = (u64)llround(bounds.width * scale_f);
    u64 new_height = (u64)llround(bounds.height * scale_f);
    if (new_width < 1) {
        new_width = 1;
    }
    if (new_height < 1) {
        new_height = 1;
    }
    if (surface_debug_enabled()) {
        fprintf(stderr, "posix-host-surface: probe %llux%llu -> %llux%llu scale=%llu\n",
                g_width, g_height, new_width, new_height, new_scale);
    }
    if (new_width == g_width && new_height == g_height && new_scale == g_scale) {
        return;
    }
    if (!reallocate_backbuffer(new_width, new_height)) {
        return; /* 할당 실패 — 이전 표면 유지 */
    }
    g_scale = new_scale;
    if (g_layer != nil) {
        g_layer.contentsScale = scale_f;
        g_layer.drawableSize = CGSizeMake((CGFloat)new_width, (CGFloat)new_height);
    }
    /* RESIZE 이벤트를 링에 직접 밀지 않는다. 전체화면 전환 같은 리사이즈
     * 폭풍에서는 링(256칸)이 이동 이벤트로 차 있을 수 있고, key_ring_push는
     * 가득 차면 **조용히 버린다** — 그 한 번이 유일한 통지라 앱은 옛 기하로
     * 영원히 그린다(안 그려진 드로어블 = 검은 띠, 어긋난 blit = 유령 프레임).
     * 대신 아래 poll이 "앱이 아는 기하 ≠ 실제 기하"를 매 프레임 검사해
     * 확인될 때까지 이벤트를 다시 만든다 — 유실이 구조적으로 불가능하다. */
    if (push_event) {
        surface_debug_log("resize");
    }
}

/* 앱이 마지막으로 통지받은(=poll이 돌려준) 표면 기하. 실제(g_*)와 다르면
 * poll이 RESIZE 이벤트를 합성한다. */

static i64 surface_resize_event_if_needed(void) {
    if (!g_ready || g_width == 0 || g_height == 0) {
        return 0;
    }
    if (g_width == g_acked_width && g_height == g_acked_height && g_scale == g_acked_scale) {
        return 0;
    }
    g_acked_width = g_width;
    g_acked_height = g_height;
    g_acked_scale = g_scale;
    u64 event = (HMJE_INPUT_KIND_SURFACE_RESIZE << HMJE_INPUT_KIND_SHIFT)
        | ((g_scale & HMJE_INPUT_SURFACE_SCALE_MASK) << HMJE_INPUT_SURFACE_SCALE_SHIFT)
        | ((g_height & HMJE_INPUT_SURFACE_SIZE_MASK) << HMJE_INPUT_SURFACE_HEIGHT_SHIFT)
        | (g_width & HMJE_INPUT_SURFACE_SIZE_MASK);
    return (i64)event;
}

/* ---- 커스텀 콘텐츠 뷰 ------------------------------------------------------ */
/* Metal 경로: 뷰는 CAMetalLayer를 backing layer로 쓰고 drawRect는 호출되지 않는다
 * (present가 직접 drawable에 blit). 폴백 경로: CGImage drawRect. */

@interface HmjeContentView : NSView
@property(nonatomic, weak) HmjeHostWindow *hostWindow;
@end

/* A record is six native little-endian u64 words followed by UTF-8 bytes. AppKit
 * sends its callbacks on the main thread, the same thread that polls this queue. */
enum { HMJE_DROP_FILE = 1, HMJE_DROP_TEXT = 2, HMJE_DROP_IMAGE = 3 };
enum { HMJE_DROP_ENTER = 1, HMJE_DROP_MOVE = 2, HMJE_DROP_LEAVE = 3, HMJE_DROP_FINISH = 4 };
void hmje_async_host_wake(void);

static u64 hmje_drop_pasteboard_kind(NSPasteboard *pasteboard) {
    if ([pasteboard canReadObjectForClasses:@[[NSURL class]]
                                options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) return HMJE_DROP_FILE;
    if ([pasteboard availableTypeFromArray:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF]] != nil) return HMJE_DROP_IMAGE;
    if ([pasteboard availableTypeFromArray:@[NSPasteboardTypeString]] != nil) return HMJE_DROP_TEXT;
    return 0;
}

static NSData *hmje_drop_payload(NSPasteboard *pasteboard, u64 kind) {
    if (kind == HMJE_DROP_TEXT) {
        NSString *value = [pasteboard stringForType:NSPasteboardTypeString];
        return value == nil ? nil : [value dataUsingEncoding:NSUTF8StringEncoding];
    }
    if (kind == HMJE_DROP_IMAGE) {
        NSData *png = [pasteboard dataForType:NSPasteboardTypePNG];
        if (png != nil) return png;
        NSData *tiff = [pasteboard dataForType:NSPasteboardTypeTIFF];
        NSBitmapImageRep *bitmap = tiff == nil ? nil : [NSBitmapImageRep imageRepWithData:tiff];
        return bitmap == nil ? nil : [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    }
    if (kind == HMJE_DROP_FILE) {
        NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[[NSURL class]]
            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        NSMutableData *paths = [NSMutableData data];
        for (NSURL *url in urls) {
            if (![url isFileURL]) continue;
            NSString *path = [[[url path] stringByStandardizingPath]
                precomposedStringWithCanonicalMapping];
            if (![path isAbsolutePath]) continue;
            NSData *bytes = [path dataUsingEncoding:NSUTF8StringEncoding];
            if (bytes == nil || [bytes length] == 0) continue;
            [paths appendData:bytes];
            const unsigned char terminator = 0;
            [paths appendBytes:&terminator length:1];
        }
        return [paths length] == 0 ? nil : paths;
    }
    return nil;
}

static void hmje_drop_enqueue(HmjeHostWindow *host, NSPoint point, u64 kind,
                              u64 phase, NSData *payload) {
    if (host == nil || kind == 0 || host->w_scale == 0) return;
    NSUInteger length = [payload length];
    if (length > (8u << 20)) return;
    u64 x = (u64)llround(fmax(0.0, point.x * (double)host->w_scale));
    u64 y = (u64)llround(fmax(0.0, point.y * (double)host->w_scale));
    if (host->w_width > 0 && x >= host->w_width) x = host->w_width - 1;
    if (host->w_height > 0 && y >= host->w_height) y = host->w_height - 1;
    u64 header[6] = {kind, phase, x, y, host->w_id, (u64)length};
    NSMutableData *record = [NSMutableData dataWithBytes:header length:sizeof(header)];
    if (length > 0) [record appendData:payload];
    if (host->w_drop_events == nil) host->w_drop_events = [NSMutableArray array];
    if ([host->w_drop_events count] >= 256) [host->w_drop_events removeObjectAtIndex:0];
    [host->w_drop_events addObject:record];
    hmje_async_host_wake();
}

/* 보조 기술(VoiceOver, 접근성 검사기, UI 테스트)이 마지막으로 창의 접근성 트리를
 * 물은 시각. 앱은 이 값으로 "접근성 클라이언트가 있는가"를 묻고(op 3), 없으면
 * 프레임마다 전체 논리 트리를 다시 게시하지 않는다. */
static u64 g_ax_last_query_us = 0;

static u64 hmje_ax_now_us(void) {
    return (u64)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u);
}

@implementation HmjeContentView

- (NSArray *)accessibilityChildren {
    g_ax_last_query_us = hmje_ax_now_us();
    return [super accessibilityChildren];
}

- (id)accessibilityHitTest:(NSPoint)point {
    g_ax_last_query_us = hmje_ax_now_us();
    return [super accessibilityHitTest:point];
}

- (BOOL)isFlipped {
    return YES; /* canonical 좌표계(원점 좌상단)와 맞춘다 */
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pasteboard = [sender draggingPasteboard];
    u64 kind = hmje_drop_pasteboard_kind(pasteboard);
    if (kind == 0) return NSDragOperationNone;
    hmje_drop_enqueue(self.hostWindow,
        [self convertPoint:[sender draggingLocation] fromView:nil], kind, HMJE_DROP_ENTER, nil);
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    u64 kind = hmje_drop_pasteboard_kind([sender draggingPasteboard]);
    if (kind == 0) return NSDragOperationNone;
    hmje_drop_enqueue(self.hostWindow,
        [self convertPoint:[sender draggingLocation] fromView:nil], kind, HMJE_DROP_MOVE, nil);
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    u64 kind = hmje_drop_pasteboard_kind([sender draggingPasteboard]);
    hmje_drop_enqueue(self.hostWindow,
        [self convertPoint:[sender draggingLocation] fromView:nil], kind, HMJE_DROP_LEAVE, nil);
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pasteboard = [sender draggingPasteboard];
    u64 kind = hmje_drop_pasteboard_kind(pasteboard);
    NSData *payload = hmje_drop_payload(pasteboard, kind);
    if (payload == nil) return NO;
    hmje_drop_enqueue(self.hostWindow,
        [self convertPoint:[sender draggingLocation] fromView:nil], kind, HMJE_DROP_FINISH, payload);
    return YES;
}

/* 키보드는 NSTextInputClient 숨은 뷰(posix_host_text_input.m)가 first responder로
 * 받는다. 콘텐츠 뷰가 클릭으로 first responder를 가져가면 그 뷰의 입력 문맥이
 * 비활성이 되어 한글 IME가 키를 삼키거나 자모 단위로 확정한다. 그 뷰가 붙기 전
 * (초기화 순간)만 받아 준다. */
- (BOOL)acceptsFirstResponder {
    HmjeHostWindow *host = self.hostWindow;
    return (host != nil && host->w_text_state != nil) ? NO : YES;
}

/* 창이 비활성일 때의 첫 클릭도 앱에 전달한다. 기본값(NO)이면 그 클릭은
 * 창을 앞으로 가져오는 데만 쓰이고 삼켜진다 — 다른 창을 보다 돌아와서
 * 카드를 누르면 한 번은 반응이 없는 것처럼 느껴진다. */
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (CALayer *)makeBackingLayer {
    if (g_device != nil) {
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = g_device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm; /* 바이트 B,G,R,A = canonical 0x00RRGGBB */
        layer.framebufferOnly = NO; /* blit encoder 대상이 되려면 필요 */
        layer.opaque = YES;         /* canonical 상위 바이트(알파)=0 무시 */
        if (self.hostWindow != nil) self.hostWindow->w_layer = layer;
        else g_layer = layer;
        return layer;
    }
    return [super makeBackingLayer];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    HmjeHostWindow *previous = hmje_route_begin(self.hostWindow);
    if (self.hostWindow != nil && g_ready) {
        surface_debug_log("view-frame");
        update_surface_geometry(YES);
    }
    hmje_route_end(previous);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    HmjeHostWindow *previous = hmje_route_begin(self.hostWindow);
    if (self.hostWindow != nil && g_ready) {
        update_surface_geometry(YES); /* 디스플레이 이동으로 레티나 배율이 바뀐 경우 */
    }
    hmje_route_end(previous);
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    HmjeHostWindow *previous = hmje_route_begin(self.hostWindow);
    [self drawRoutedRect];
    hmje_route_end(previous);
}

- (void)drawRoutedRect {
    /* Metal 경로에서는 호출되지 않는다(백킹 레이어가 CAMetalLayer). 폴백 전용. */
    if (g_backbuffer == NULL || g_width == 0 || g_height == 0) {
        [[NSColor blackColor] setFill];
        NSRectFill([self bounds]);
        return;
    }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, g_backbuffer, (size_t)(g_pitch * g_height), NULL);
    /* AlphaNoneSkipFirst + Little => in-memory byte order B,G,R,X, 즉 LE u32로 읽으면
     * 0x00RRGGBB — runtime/user/graphics.hmje가 정의한 canonical 포맷과 정확히 일치. */
    CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
    CGImageRef image = CGImageCreate((size_t)g_width, (size_t)g_height, 8, 32, (size_t)g_pitch,
                                      colorSpace, bitmapInfo, provider, NULL, false, kCGRenderingIntentDefault);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, (CGFloat)[self bounds].size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, [self bounds].size.width, [self bounds].size.height), image);
    CGContextRestoreGState(ctx);
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);
}

@end

/* ---- macOS 접근성 ------------------------------------------------------- */
/* iOS 백엔드와 같은 syscall 109 wire ABI를 AppKit의 표준
 * NSAccessibilityElement로 바꾼다. 실제 포커스·편집·렌더링은 계속
 * HMJE가 소유하고, 여기서는 VoiceOver/Accessibility Inspector에 의미·위치·
 * 표준 액션만 노출한다. */
#define HMJE_AX_NODE_WORDS 18
#define HMJE_AX_MAX_NODES 8192
#define HMJE_AX_MAX_TEXT_BYTES (1024 * 1024)

static os_unfair_lock g_ax_lock = OS_UNFAIR_LOCK_INIT;

static BOOL hmje_ax_enqueue_window(u64 identifier, u64 action) {
    os_unfair_lock_lock(&g_ax_lock);
    if (![g_ax_current containsObject:@(identifier)]) {
        os_unfair_lock_unlock(&g_ax_lock);
        return NO;
    }
    unsigned next = (g_ax_write + 1) % 64;
    if (next == g_ax_read) {
        os_unfair_lock_unlock(&g_ax_lock);
        return NO;
    }
    g_ax_actions[g_ax_write] = (HmjeAXAction){identifier, action};
    g_ax_write = next;
    os_unfair_lock_unlock(&g_ax_lock);
    return YES;
}

/* 접근성 동작은 요소가 속한 창의 큐로 간다(창마다 자기 루프가 읽는다). */
static BOOL hmje_ax_enqueue(HmjeHostWindow *window, u64 identifier, u64 action) {
    if (window == nil) return NO;
    HmjeHostWindow *previous = hmje_route_begin(window);
    BOOL queued = hmje_ax_enqueue_window(identifier, action);
    hmje_route_end(previous);
    return queued;
}

@interface HmjeAccessibilityElement : NSAccessibilityElement
/* Published frame in the flipped content view's points (origin top-left). */
@property(nonatomic, assign) NSRect semanticFrame;
@property(nonatomic, assign) u64 semanticIdentifier;
@property(nonatomic, assign) u64 semanticActions;
@property(nonatomic, weak) HmjeHostWindow *hostWindow;
@end

@implementation HmjeAccessibilityElement
/* NSAccessibilityElement reads accessibilityFrameInParentSpace as bottom-left even
 * when the parent view is flipped, so a top-left rect set there would be vertically
 * mirrored. Convert through the view instead; this also stays right
 * when the window resizes or moves between snapshots. */
- (NSRect)accessibilityFrame {
    id parent = self.accessibilityParent;
    if (![parent isKindOfClass:[NSView class]] || ((NSView *)parent).window == nil) return NSZeroRect;
    return NSAccessibilityFrameInView((NSView *)parent, self.semanticFrame);
}
- (NSRect)accessibilityFrameInParentSpace {
    NSRect frame = self.semanticFrame;
    id parent = self.accessibilityParent;
    if ([parent isKindOfClass:[NSView class]] && ((NSView *)parent).isFlipped)
        frame.origin.y = NSHeight(((NSView *)parent).bounds) - NSMaxY(frame);
    return frame;
}
- (BOOL)accessibilityPerformPress {
    if (self.semanticActions & 1) return hmje_ax_enqueue(self.hostWindow, self.semanticIdentifier, 1);
    if (self.semanticActions & 2) return hmje_ax_enqueue(self.hostWindow, self.semanticIdentifier, 2);
    return NO;
}
- (BOOL)accessibilityPerformIncrement {
    return (self.semanticActions & 4) && hmje_ax_enqueue(self.hostWindow, self.semanticIdentifier, 4);
}
- (BOOL)accessibilityPerformDecrement {
    return (self.semanticActions & 8) && hmje_ax_enqueue(self.hostWindow, self.semanticIdentifier, 8);
}
- (BOOL)isAccessibilitySelectorAllowed:(SEL)selector {
    if (selector == @selector(accessibilityPerformPress))
        return (self.semanticActions & 3) != 0;
    if (selector == @selector(accessibilityPerformIncrement))
        return (self.semanticActions & 4) != 0;
    if (selector == @selector(accessibilityPerformDecrement))
        return (self.semanticActions & 8) != 0;
    return [super isAccessibilitySelectorAllowed:selector];
}
@end

static NSString *hmje_ax_text(u64 address, u64 length) {
    if (length == 0) return @"";
    if (address == 0 || length > HMJE_AX_MAX_TEXT_BYTES) return nil;
    return [[NSString alloc] initWithBytes:(const void *)(uintptr_t)address
                                   length:(NSUInteger)length
                                 encoding:NSUTF8StringEncoding];
}

static NSAccessibilityRole hmje_ax_role(u64 role) {
    if (role == 1) return NSAccessibilityStaticTextRole;
    if (role == 2 || role == 4 || role == 5) return NSAccessibilityButtonRole;
    if (role == 3) return NSAccessibilityTextFieldRole;
    if (role == 6) return NSAccessibilitySliderRole;
    return NSAccessibilityGroupRole;
}

i64 hmje_posix_ui_accessibility(i64 operation, i64 address, i64 length) {
    if (operation == 0) return 1;
    if (operation == 3) {
        /* 1 = 보조 기술이 켜져 있거나 최근 10초 안에 트리를 물었다, 0 = 없다. */
        /* 앱 루프는 NSApplication.run의 바깥에서 매 프레임 이 경계를 부른다.
         * NSWorkspace 내부의 autoreleased 객체도 호출마다 회수해야 한다.
         * 이 범위에서는 창 흐름을 전환하지 않는다. */
        @autoreleasepool {
            if ([[NSWorkspace sharedWorkspace] isVoiceOverEnabled]) return 1;
            if (g_ax_last_query_us != 0 && hmje_ax_now_us() - g_ax_last_query_us < 10000000u) return 1;
            return 0;
        }
    }
    if (operation == 2) {
        if (!address || length < (i64)sizeof(HmjeAXAction)) return -22;
        os_unfair_lock_lock(&g_ax_lock);
        if (g_ax_read == g_ax_write) {
            os_unfair_lock_unlock(&g_ax_lock);
            return 0;
        }
        HmjeAXAction action = g_ax_actions[g_ax_read];
        g_ax_read = (g_ax_read + 1) % 64;
        os_unfair_lock_unlock(&g_ax_lock);
        memcpy((void *)(uintptr_t)(u64)address, &action, sizeof(action));
        return (i64)sizeof(action);
    }
    if (operation != 1) return HMJE_POSIX_ENOSYS_LOCAL;

    const size_t stride = HMJE_AX_NODE_WORDS * sizeof(u64);
    if (length < 0 || (u64)length % stride != 0 || (u64)length / stride > HMJE_AX_MAX_NODES)
        return -22;
    if (length != 0 && address == 0) return -22;

    size_t count = (size_t)length / stride;
    const u64 *records = (const u64 *)(uintptr_t)(u64)address;
    @autoreleasepool {
        NSMutableArray<NSDictionary *> *snapshot = [NSMutableArray arrayWithCapacity:count];
        NSMutableSet<NSNumber *> *keys = [NSMutableSet setWithCapacity:count];
        for (size_t i = 0; i < count; ++i) {
            const u64 *r = records + i * HMJE_AX_NODE_WORDS;
            if (r[0] == 0 || r[2] < 1 || r[2] > 7 || r[8] > 1000000 || r[9] > 1000000
                || (i64)r[6] < -1000000 || (i64)r[7] < -1000000
                || (i64)r[6] > 1000000 || (i64)r[7] > 1000000
                || [keys containsObject:@(r[0])]) return -22;
            NSString *label = hmje_ax_text(r[10], r[11]);
            NSString *value = (r[3] & 32) ? @"" : hmje_ax_text(r[12], r[13]);
            if (label == nil || value == nil) return -22;
            [keys addObject:@(r[0])];
            [snapshot addObject:@{@"id": @(r[0]), @"role": @(r[2]), @"state": @(r[3]),
                @"actions": @(r[4]), @"label": label, @"value": value,
                @"frame": [NSValue valueWithRect:NSMakeRect((i64)r[6], (i64)r[7], r[8], r[9])]}];
        }

        HmjeHostWindow *hostWindow = HMJE_WIN;
        void (^applySnapshot)(void) = ^{
            if (g_view == nil) return;
            if (g_ax_elements == nil) g_ax_elements = [NSMutableDictionary dictionary];
            BOOL topologyChanged = ![g_ax_current isEqualToSet:keys];
            NSMutableDictionary<NSNumber *, HmjeAccessibilityElement *> *next =
                [NSMutableDictionary dictionaryWithCapacity:count];
            NSMutableArray *ordered = [NSMutableArray arrayWithCapacity:count];
            for (NSDictionary *record in snapshot) {
                NSNumber *key = record[@"id"];
                u64 role = [record[@"role"] unsignedLongLongValue];
                u64 state = [record[@"state"] unsignedLongLongValue];
                u64 actions = (state & 1) ? 0 : [record[@"actions"] unsignedLongLongValue];
                /* HmjeUI publishes frames in logical points (layout runs at
                 * pixel size / 그리기명령재생배율, the surface scale), the same unit as
                 * this flipped view's bounds. Dividing by g_scale again would halve every
                 * frame on Retina. iOS uses them as points too; Android
                 * multiplies by its scale to get pixels. */
                NSRect pointFrame = [record[@"frame"] rectValue];
                HmjeAccessibilityElement *element = g_ax_elements[key];
                if (element == nil) element = [[HmjeAccessibilityElement alloc] init];
                element.semanticIdentifier = key.unsignedLongLongValue;
                element.semanticActions = actions;
                element.hostWindow = hostWindow;
                element.accessibilityElement = YES;
                element.accessibilityParent = g_view;
                element.accessibilityRole = hmje_ax_role(role);
                element.accessibilitySubrole = (role == 3 && (state & 32))
                    ? NSAccessibilitySecureTextFieldSubrole : nil;
                element.accessibilityLabel = record[@"label"];
                element.accessibilityValue = record[@"value"];
                element.accessibilityIdentifier = [NSString stringWithFormat:@"hmje.%llu", element.semanticIdentifier];
                element.semanticFrame = pointFrame;
                element.accessibilityEnabled = (state & 1) == 0;
                element.accessibilitySelected = (state & 2) != 0;
                element.accessibilityFocused = (state & 16) != 0;
                if (role == 3)
                    element.accessibilityHelp = (state & 4) ? @"여러 줄 텍스트 편집" : @"텍스트 편집";
                next[key] = element;
                [ordered addObject:element];
            }

            g_ax_elements = next;
            os_unfair_lock_lock(&g_ax_lock);
            g_ax_current = [keys copy];
            os_unfair_lock_unlock(&g_ax_lock);
            g_view.accessibilityElement = NO;
            g_view.accessibilityChildren = ordered;
            g_view.accessibilityChildrenInNavigationOrder = ordered;
            if (topologyChanged)
                NSAccessibilityPostNotification(g_view, NSAccessibilityLayoutChangedNotification);
        };
        if ([NSThread isMainThread]) applySnapshot();
        else dispatch_sync(dispatch_get_main_queue(), applySnapshot);
        return (i64)count;
    }
}

/* ---- 표준 macOS 앱 생명주기 --------------------------------------------- */

@interface HmjeApplicationDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

static void request_application_quit(void);
static void app_open_url_enqueue(NSString *url);

@implementation HmjeApplicationDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    /* Native embedding tools that never enter HMJE's input loop retain normal
     * AppKit termination. A running HMJE app receives one close event so its
     * language-level loop can unwind and release resources first. */
    if (!g_runtime_polling && !g_window_events_aware) return NSTerminateNow;
    if (g_window_events_aware) {
        /* 창마다 종료 표식이 붙은 닫기 요청을 받는다. 저장 안 한 창은 거부할
         * 수 있고(UI_WINDOW_OP_QUIT_CANCEL), 창이 모두 닫히면 주 흐름이 끝낸다. */
        request_application_quit();
        return NSTerminateCancel;
    }
    /* Let HMJE unwind its app loop and release graphics/task resources before
     * process exit.  A second request after delivery is still cancelled: the
     * runtime exits itself once cleanup completes. */
    g_close_requested = YES;
    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    /* 창 흐름을 쓰는 앱은 마지막 창 정책(UI_WINDOW_OP_POLICY)을 HMJE가 적용한다. */
    return g_window_events_aware ? NO : YES;
}

/* 닫기 단추·⌘W(창 메뉴 performClose:)·Mission Control 닫기가 모두 여기로 온다.
 * 창 사건을 아는 앱은 거부할 수 있도록 닫기 요청만 보내고 창은 두고 간다. */
- (BOOL)windowShouldClose:(NSWindow *)sender {
    HmjeHostWindow *window = hmje_window_for_nswindow(sender);
    if (!g_window_events_aware || window == nil) return YES;
    if (!window->w_closing) window_push_event(window, HMJE_WINDOW_EVENT_CLOSE_REQUEST);
    return NO;
}

- (void)windowWillClose:(NSNotification *)notification {
    if (g_window_events_aware) return;
    HmjeHostWindow *window = hmje_window_for_nswindow(notification.object);
    if (window != nil) {
        /* NSApplication의 run을 쓰지 않는 HMJE 이벤트 펌프에서는
         * applicationShouldTerminateAfterLastWindowClosed:만으로 종료 요청이
         * 전달되지 않으므로 표준 terminate: 경로를 명시적으로 시작한다. */
        [NSApp terminate:nil];
    }
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    HmjeHostWindow *window = hmje_window_for_nswindow(notification.object);
    if (window != nil) window->w_key_active = YES;
    if (window != nil && !window->w_closing) g_front_window = window;
    if (g_window_events_aware && window != nil && !window->w_closing)
        window_push_event(window, HMJE_WINDOW_EVENT_BECAME_KEY);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    HmjeHostWindow *window = hmje_window_for_nswindow(notification.object);
    if (window != nil) window->w_key_active = NO;
    if (g_window_events_aware && window != nil && !window->w_closing)
        window_push_event(window, HMJE_WINDOW_EVENT_RESIGNED_KEY);
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)hasVisibleWindows {
    (void)sender;
    if (hasVisibleWindows) return YES;
    for (HmjeHostWindow *window in g_windows) {
        if (!window->w_closing && window->w_window != nil) {
            [window->w_window makeKeyAndOrderFront:nil];
            return YES;
        }
    }
    /* 창이 하나도 없다(계속 실행 정책). 주 흐름이 다시 열기 처리기를 부른다. */
    if (g_window_events_aware) g_reopen_requested = YES;
    return YES;
}

/* 앱의 URL 스킴(Info.plist CFBundleURLTypes)으로 연 주소. 앱이 돌고 있으면 바로, 꺼져
 * 있었으면 운영체제가 앱을 띄운 뒤 finishLaunching 다음 첫 사건 펌프에서 온다(kAEGetURL을
 * AppKit이 이 메서드로 바꿔 준다). 주소는 큐에 쌓고 앱 루프가 SYS_APP_OPEN_URL로 꺼내
 * `URL이 열리면`을 부른다 — 앱 코드가 아직 처리기를 등록하지 않았어도 잃지 않는다.
 * 문서 파일(file:)은 이 계약이 아니다(CFBundleDocumentTypes를 싣지 않는다). */
- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
    (void)application;
    for (NSURL *url in urls) {
        if (url == nil || [url isFileURL]) continue;
        NSString *text = [url absoluteString];
        if ([text length] > 0) app_open_url_enqueue(text);
    }
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    if (!g_window_events_aware && g_window != nil && ![g_window isVisible]) {
        [g_window makeKeyAndOrderFront:nil];
    }
}

@end

static void install_standard_application_menu(NSString *applicationName) {
    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *applicationMenuItem = [[NSMenuItem alloc] initWithTitle:@""
                                                                 action:nil
                                                          keyEquivalent:@""];
    [mainMenu addItem:applicationMenuItem];

    NSMenu *applicationMenu = [[NSMenu alloc] initWithTitle:applicationName];
    NSString *hideTitle = [NSString stringWithFormat:@"%@ 숨기기", applicationName];
    NSMenuItem *hideItem = [[NSMenuItem alloc] initWithTitle:hideTitle
                                                    action:@selector(hide:)
                                             keyEquivalent:@"h"];
    [hideItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [applicationMenu addItem:hideItem];
    [applicationMenu addItem:[NSMenuItem separatorItem]];
    NSString *quitTitle = [NSString stringWithFormat:@"%@ 종료", applicationName];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                     action:@selector(terminate:)
                                              keyEquivalent:@"q"];
    [quitItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [applicationMenu addItem:quitItem];
    [applicationMenuItem setSubmenu:applicationMenu];

    NSMenuItem *editMenuItem = [[NSMenuItem alloc] initWithTitle:@"편집" action:nil keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"편집"];
    NSArray<NSMenuItem *> *editItems = @[
        [[NSMenuItem alloc] initWithTitle:@"실행 취소" action:@selector(undo:) keyEquivalent:@"z"],
        [[NSMenuItem alloc] initWithTitle:@"다시 실행" action:@selector(redo:) keyEquivalent:@"Z"],
        [[NSMenuItem alloc] initWithTitle:@"잘라내기" action:@selector(cut:) keyEquivalent:@"x"],
        [[NSMenuItem alloc] initWithTitle:@"복사" action:@selector(copy:) keyEquivalent:@"c"],
        [[NSMenuItem alloc] initWithTitle:@"붙여넣기" action:@selector(paste:) keyEquivalent:@"v"],
        [[NSMenuItem alloc] initWithTitle:@"전체 선택" action:@selector(selectAll:) keyEquivalent:@"a"]
    ];
    for (NSMenuItem *item in editItems) [editMenu addItem:item];
    [editMenuItem setSubmenu:editMenu];
    [mainMenu addItem:editMenuItem];

    NSMenuItem *windowMenuItem = [[NSMenuItem alloc] initWithTitle:@"창" action:nil keyEquivalent:@""];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"창"];
    NSMenuItem *minimizeItem = [[NSMenuItem alloc] initWithTitle:@"최소화"
                                                         action:@selector(performMiniaturize:)
                                                  keyEquivalent:@"m"];
    NSMenuItem *closeItem = [[NSMenuItem alloc] initWithTitle:@"창 닫기"
                                                      action:@selector(performClose:)
                                               keyEquivalent:@"w"];
    [windowMenu addItem:minimizeItem];
    [windowMenu addItem:closeItem];
    [windowMenuItem setSubmenu:windowMenu];
    [mainMenu addItem:windowMenuItem];
    [NSApp setWindowsMenu:windowMenu];
    [NSApp setMainMenu:mainMenu];
}

i64 hmje_posix_ui_clipboard(i64 operation, i64 address, i64 length) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        if (operation == 0) {
            NSString *text = [pasteboard stringForType:NSPasteboardTypeString] ?: @"";
            NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
            NSUInteger count = utf8.length;
            if (address != 0 && length >= 0 && (u64)length >= (u64)count && count > 0)
                memcpy((void *)(uintptr_t)(u64)address, utf8.bytes, count);
            return (i64)count;
        }
        if (operation == 1) {
            if (length < 0 || (address == 0 && length != 0)) return -22;
            NSString *text = [[NSString alloc] initWithBytes:(const void *)(uintptr_t)(u64)address
                                                      length:(NSUInteger)length
                                                    encoding:NSUTF8StringEncoding];
            if (text == nil) return -22;
            [pasteboard clearContents];
            return [pasteboard setString:text forType:NSPasteboardTypeString] ? length : -5;
        }
        return -22;
    }
}

/* ---- 창 흐름(fiber) ------------------------------------------------------
 * 창마다 HmjeUI 앱 루프가 하나씩 돈다. 루프는 지역 변수로 뷰 트리·포커스·
 * 텍스트 세션을 들고 있으므로 창마다 스택이 따로 있어야 한다. AppKit은 메인
 * 스레드만 허용하므로 스레드 대신 같은 메인 스레드 안의 협력 흐름을 쓴다.
 * 흐름은 대기(SYS_SLEEP_MS → hmje_async_host_wait_ms)와 창 연산에서만 바뀐다 —
 * 한 프레임(GPU BEGIN..END_PRESENT) 도중에는 절대 바뀌지 않는다.
 *
 * 주 흐름(원래 메인 스택)은 첫 창(ensure_window)의 루프를 돌리거나,
 * 창을 연 뒤 UI_WINDOW_OP_RUN에서 기다린다. 새 창은 UI_WINDOW_OP_OPEN이 만든
 * 흐름에서 HmjeUI 진입 함수를 부른다. 흐름이 끝나면(UI_WINDOW_OP_FINISH) 그
 * 스택은 다른 흐름이 치운다. */
#include "posix_host_fiber.h"
#include <sys/mman.h>
#include "guarded_stack.h"

enum { HMJE_FIBER_NEW, HMJE_FIBER_RUNNING, HMJE_FIBER_WAITING, HMJE_FIBER_PARKED, HMJE_FIBER_DEAD };

@interface HmjeFiber : NSObject {
@public
    void *sp;
    void *stack;
    size_t stack_size;
    HmjeHostWindow *win;
    void *(*entry)(void *);
    void *arg;
    int state;
    double deadline;
    u64 async_seen;
    /* 대기를 시작할 때 본 창의 주사 번호(hmje_vsync_*). */
    u64 vsync_seen;
    BOOL is_main;
    /* UI_WINDOW_OP_WAIT 안에서만 흐름을 바꾼다. 앱 코드의 SYS_SLEEP_MS(막는 대기)는
     * 이 흐름만 재운다 — 한 창의 프레임 도중에 다른 창의 프레임이 끼면 안 된다. */
    BOOL switchable;
    /* OPEN 안에서 기다리는 여는 흐름. 새 흐름이 STARTED를 부르면 곧장 여기로 돌아간다. */
    __unsafe_unretained HmjeFiber *opener;
}
@end

@implementation HmjeFiber
- (void)dealloc {
    if (stack != NULL) munmap(stack, stack_size);
}
@end

#define HMJE_FIBER_STACK_BYTES ((size_t)64 << 20)
#define HMJE_FIBER_GUARD_BYTES ((size_t)64 << 10)

static HmjeFiber *g_fiber_main = nil;
static HmjeFiber *g_fiber_current = nil;
static NSMutableArray<HmjeFiber *> *g_fibers = nil;       /* 주 흐름을 뺀 살아 있는 흐름 */
static NSMutableArray<HmjeFiber *> *g_fiber_zombies = nil; /* 끝났지만 스택을 아직 못 치운 흐름 */
static u64 g_async_wake_count = 0;

static double hmje_now_seconds(void) {
    return [[NSProcessInfo processInfo] systemUptime];
}

static void hmje_fibers_init(void) {
    if (g_fiber_main != nil) return;
    g_fiber_main = [[HmjeFiber alloc] init];
    g_fiber_main->is_main = YES;
    g_fiber_main->state = HMJE_FIBER_RUNNING;
    g_fiber_main->win = g_win; /* 이미 만든 첫 창(표시 함수 앱)은 주 흐름의 창이다 */
    g_fiber_current = g_fiber_main;
    g_fibers = [NSMutableArray array];
    g_fiber_zombies = [NSMutableArray array];
    if (g_windows == nil) g_windows = [NSMutableArray array];
}

/* 창 흐름을 쓰기 시작했는가. 한 번도 쓰지 않은 앱은 기본 대기 경로를 탄다. */
static BOOL hmje_fibers_active(void) {
    return g_fibers != nil && ([g_fibers count] > 0 || g_fiber_main->state != HMJE_FIBER_RUNNING
                               || g_window_events_aware);
}

static u64 hmje_live_window_count(void) {
    u64 count = 0;
    for (HmjeHostWindow *window in g_windows) {
        if (!window->w_closing) count++;
    }
    return count;
}

static BOOL hmje_window_has_input(HmjeHostWindow *window) {
    if (window == nil) return NO;
    if (window->w_key_ring_head != window->w_key_ring_tail) return YES;
    if (window->w_pointer_samples.head != window->w_pointer_samples.tail) return YES;
    if (window->w_ax_read != window->w_ax_write) return YES;
    if (window->w_ready && (window->w_width != window->w_acked_width ||
                            window->w_height != window->w_acked_height ||
                            window->w_scale != window->w_acked_scale)) return YES;
    return NO;
}

/* 주 흐름이 RUN/FINISH에서 돌아갈 때가 됐는가. */
static BOOL hmje_main_should_return(void) {
    if (hmje_live_window_count() > 0) return NO;
    if (g_quit_requested) return YES;
    if (g_last_window_policy == 1) return YES;
    return g_reopen_requested;
}

/* ---- 화면 주사 페이싱(vsync) ------------------------------------------------
 * 창마다 뷰의 CADisplayLink(macOS 14+)가 주사마다 w_vsync_tick을 올리고 응용 정의 사건
 * (부표 111)으로 네이티브 대기를 깨운다. 앱 루프에는 대기 결과 bit1(주사)로 알린다.
 * 애니메이션·관성 같은 연속 프레임은 디스플레이 주사에 맞춰 돈다.
 *
 * 입력(트랙패드 표본·키)은 곧바로 대기를 끝낸다. 입력마다 그리면 nextDrawable이
 * 다음 주사까지 막아 present가 주사 직후에 나간다.
 *
 * 링크는 present가 있을 때만 돈다. present 없이 주사 4번이 지나면 멈추고,
 * 다음 present가 다시 켠다. 링크가 멈춘 창의 첫 입력은 곧바로 그린다.
 * CADisplayLink가 없는 시스템은 사건마다 그린다.
 * HMJE_POSIX_NO_VSYNC=1이면 끈다(비교 측정용). */




#define HMJE_VSYNC_WAKE_SUBTYPE 111
#define HMJE_VSYNC_IDLE_TICKS 4

static BOOL hmje_vsync_disabled(void) {
    static int disabled = -1;
    if (disabled < 0) {
        const char *value = getenv("HMJE_POSIX_NO_VSYNC");
        disabled = (value != NULL && value[0] == '1') ? 1 : 0;
    }
    return disabled == 1;
}

@interface HmjeVsyncTarget : NSObject
@property (nonatomic, weak) HmjeHostWindow *hostWindow;
- (void)tick:(id)link;
@end

@implementation HmjeVsyncTarget
- (void)tick:(id)link {
    HmjeHostWindow *window = self.hostWindow;
    if (window == nil || window->w_closing) {
        [link setPaused:YES];
        return;
    }
    window->w_vsync_tick++;
    static int debug = -1;
    if (debug < 0) debug = getenv("HMJE_POSIX_DEBUG_VSYNC") != NULL ? 1 : 0;
    if (debug == 1)
        fprintf(stderr, "posix-host-tick: t=%llu n=%llu\n", (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u), (unsigned long long)window->w_vsync_tick);
    if (window->w_vsync_tick - window->w_present_tick > HMJE_VSYNC_IDLE_TICKS) {
        [link setPaused:YES];
        window->w_vsync_running = NO;
    }
    /* 링크 콜백은 주 런루프에서 돌지만 nextEventMatchingMask는 사건이 와야 돌아온다. */
    @autoreleasepool {
        NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:HMJE_VSYNC_WAKE_SUBTYPE
                                              data1:(NSInteger)window->w_id
                                              data2:0];
        [NSApp postEvent:wake atStart:NO];
    }
}
@end

/* present한 창: 이 주사를 적고, 링크가 없으면 만들고 멈춰 있으면 켠다. */
static void hmje_vsync_presented(HmjeHostWindow *window) {
    if (window == nil || window->w_closing || window->w_view == nil || hmje_vsync_disabled()) return;
    if (window->w_display_link == nil) {
        if (@available(macOS 14.0, *)) {
            HmjeVsyncTarget *target = [[HmjeVsyncTarget alloc] init];
            target.hostWindow = window;
            CADisplayLink *link = [window->w_view displayLinkWithTarget:target selector:@selector(tick:)];
            /* ProMotion: 디스플레이 최대 주사율을 청한다(청하지 않으면 시스템이 낮출 수 있다). */
            float maximum = 120.0f;
            NSScreen *screen = window->w_window.screen != nil ? window->w_window.screen : [NSScreen mainScreen];
            if (screen != nil && screen.maximumFramesPerSecond > 0) maximum = (float)screen.maximumFramesPerSecond;
            link.preferredFrameRateRange = CAFrameRateRangeMake(maximum > 60.0f ? 60.0f : maximum, maximum, maximum);
            [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
            window->w_display_link = link;
            window->w_vsync_running = YES;
        } else {
            return;
        }
    }
    window->w_present_tick = window->w_vsync_tick;
    if (!window->w_vsync_running) {
        window->w_vsync_running = YES;
        [window->w_display_link setPaused:NO];
    }
}

static void hmje_vsync_stop(HmjeHostWindow *window) {
    if (window == nil || window->w_display_link == nil) return;
    [window->w_display_link invalidate];
    window->w_display_link = nil;
    window->w_vsync_running = NO;
}

static BOOL hmje_fiber_ready(HmjeFiber *fiber, double now) {
    switch (fiber->state) {
    case HMJE_FIBER_NEW:
        return YES;
    case HMJE_FIBER_WAITING:
        if (now >= fiber->deadline || fiber->async_seen != g_async_wake_count) return YES;
        if (fiber->win != nil && fiber->win->w_vsync_tick != fiber->vsync_seen) return YES;
        return hmje_window_has_input(fiber->win);
    case HMJE_FIBER_PARKED:
        return hmje_main_should_return();
    default:
        return NO;
    }
}

static void hmje_fiber_switch_to(HmjeFiber *next) {
    HmjeFiber *previous = g_fiber_current;
    if (next == previous) {
        next->state = HMJE_FIBER_RUNNING;
        return;
    }
    g_fiber_current = next;
    next->state = HMJE_FIBER_RUNNING;
    g_win = next->win;
    if (getenv("HMJE_POSIX_DEBUG_FIBERS") != NULL)
        fprintf(stderr, "posix-host-fiber: %llu -> %llu\n", previous->win ? previous->win->w_id : 0,
                next->win ? next->win->w_id : 0);
    void **save = &previous->sp;
    void *load = next->sp;
    hmje_fiber_switch(save, load);
    /* 누군가 이 흐름을 다시 골랐다. g_fiber_current·g_win은 고른 쪽이 맞춰 두었다. */
}

/* 끝난 흐름의 스택은 다른 흐름 위에서만 치울 수 있다. */
static void hmje_fiber_reap(void) {
    if ([g_fiber_zombies count] == 0) return;
    NSMutableArray<HmjeFiber *> *keep = [NSMutableArray array];
    for (HmjeFiber *fiber in g_fiber_zombies) {
        if (fiber == g_fiber_current) [keep addObject:fiber];
    }
    [g_fiber_zombies setArray:keep];
}

/* 돌 흐름을 고른다. 지금 흐름 다음부터 돌아가며 찾아 한 흐름이 독차지하지 않게 한다. */
static HmjeFiber *hmje_fiber_pick(HmjeFiber *exclude, double now) {
    NSMutableArray<HmjeFiber *> *order = [NSMutableArray arrayWithArray:g_fibers];
    [order addObject:g_fiber_main];
    NSUInteger count = [order count];
    NSUInteger start = 0;
    NSUInteger index = [order indexOfObjectIdenticalTo:g_fiber_current];
    if (index != NSNotFound) start = index + 1;
    for (NSUInteger step = 0; step < count; step++) {
        HmjeFiber *candidate = order[(start + step) % count];
        if (candidate == exclude) continue;
        if (hmje_fiber_ready(candidate, now)) return candidate;
    }
    return nil;
}

static double hmje_fiber_earliest_deadline(double fallback) {
    double earliest = fallback;
    for (HmjeFiber *fiber in g_fibers) {
        if (fiber->state == HMJE_FIBER_WAITING && fiber->deadline < earliest) earliest = fiber->deadline;
    }
    if (g_fiber_main->state == HMJE_FIBER_WAITING && g_fiber_main->deadline < earliest)
        earliest = g_fiber_main->deadline;
    return earliest;
}

static void pump_events(void);
extern int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms);

/* 네이티브 이벤트가 오거나 until까지 잔다(이벤트는 꺼내지 않는다 — 곧 펌프가 처리). */
static void hmje_native_block_until(double until) {
    double now = hmje_now_seconds();
    if (until <= now) return;
    if (until - now > 1.0) until = now + 1.0;
    @autoreleasepool {
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:until - now];
        (void)[NSApp nextEventMatchingMask:NSEventMaskAny
                                  untilDate:deadline
                                     inMode:NSDefaultRunLoopMode
                                    dequeue:NO];
    }
}

/* 셀프테스트: 창이 하나도 없는 채 HMJE_POSIX_SELFTEST_REOPEN 밀리초가 지나면 Dock
 * 아이콘을 누른 것처럼 applicationShouldHandleReopen:을 한 번 부른다(계속 실행 정책의
 * 다시 열기 경로 검증). */
static void selftest_maybe_reopen(void) {
    static double zero_since = 0;
    static BOOL done = NO;
    const char *spec = getenv("HMJE_POSIX_SELFTEST_REOPEN");
    if (spec == NULL || done) return;
    if (hmje_live_window_count() > 0 || !g_window_events_aware || g_application_delegate == nil) {
        zero_since = 0;
        return;
    }
    double now = hmje_now_seconds();
    if (zero_since == 0) zero_since = now;
    if ((now - zero_since) * 1000.0 < atof(spec)) return;
    done = YES;
    fprintf(stderr, "posix-host-window: selftest reopen\n");
    (void)[g_application_delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
}

/* 지금 흐름을 state로 두고 다른 흐름을 돌린다. 지금 흐름이 다시 준비될 때 돌아온다.
 * 죽은 흐름(state=DEAD)은 돌아오지 않는다. */
static void hmje_fiber_block(int state, double deadline) {
    HmjeFiber *me = g_fiber_current;
    me->state = state;
    me->deadline = deadline;
    for (;;) {
        pump_events();
        hmje_fiber_reap();
        double now = hmje_now_seconds();
        HmjeFiber *next = hmje_fiber_pick(me, now);
        if (next != nil) {
            hmje_fiber_switch_to(next);
            if (me->state == HMJE_FIBER_RUNNING) return;
            continue;
        }
        if (state != HMJE_FIBER_DEAD && hmje_fiber_ready(me, now)) {
            me->state = HMJE_FIBER_RUNNING;
            return;
        }
        double until = hmje_fiber_earliest_deadline(now + 0.25);
        if (state == HMJE_FIBER_WAITING && deadline < until) until = deadline;
        selftest_maybe_reopen();
        if (hmje_fiber_ready(me, hmje_now_seconds()) && state != HMJE_FIBER_DEAD) continue;
        hmje_native_block_until(until);
    }
}

void hmje_fiber_entry_c(void *argument) {
    __unsafe_unretained HmjeFiber *fiber = (__bridge HmjeFiber *)argument;
    void *(*entry)(void *) = fiber->entry;
    void *entry_argument = fiber->arg;
    fiber = nil;
    (void)entry(entry_argument);
    /* HmjeUI 진입 함수는 창 루프가 끝나면 FINISH를 부르므로 여기 오지 않는다.
     * 오더라도(진입 함수가 그냥 돌아옴) 창을 닫고 흐름을 끝낸다. */
    extern i64 hmje_posix_ui_window(i64 operation, i64 argument1, i64 argument2);
    (void)hmje_posix_ui_window(7, 0, 0);
    abort();
}

static HmjeFiber *hmje_fiber_create(HmjeHostWindow *window, u64 entry, u64 argument) {
    HmjeGuardedStack stack = {0};
    if (hmje_guarded_stack_allocate(&stack, HMJE_FIBER_STACK_BYTES,
                                   HMJE_FIBER_GUARD_BYTES) != 0) return nil;
    HmjeFiber *fiber = [[HmjeFiber alloc] init];
    if (fiber == nil) { hmje_guarded_stack_release(&stack); return nil; }
    fiber->stack = stack.mapping;
    fiber->stack_size = stack.mapping_size;
    fiber->win = window;
    fiber->entry = (void *(*)(void *))(uintptr_t)entry;
    fiber->arg = (void *)(uintptr_t)argument;
    fiber->state = HMJE_FIBER_NEW;
    fiber->async_seen = g_async_wake_count;
    fiber->sp = hmje_fiber_prepare((char *)stack.usable + stack.usable_size, (__bridge void *)fiber);
    return fiber;
}

/* async_host_runtime.c의 weak hook. 완전 유휴 앱은 pthread condition만
 * 기다리면 AppKit 키/포인터/Dock 이벤트에 깨지 못한다. 메인 스레드에서
 * native queue를 기다리고, worker 완료는 application-defined 이벤트를 넣어
 * 같은 대기를 깨운다. 이벤트는 dequeue하지 않아 다음 입력 poll이 처리한다.
 * 창 흐름이 있으면 기다리는 동안 다른 창의 흐름을 돌린다. */
int32_t hmje_async_host_wait_ms(uint64_t timeout_ms) {
    if (NSApp == nil || ![NSThread isMainThread]) return -1;
    if (hmje_fibers_active() && g_fiber_current->switchable) {
        HmjeFiber *me = g_fiber_current;
        me->async_seen = g_async_wake_count;
        me->vsync_seen = me->win != nil ? me->win->w_vsync_tick : 0;
        hmje_fiber_block(HMJE_FIBER_WAITING, hmje_now_seconds() + (double)timeout_ms / 1000.0);
        int32_t woke = hmje_window_has_input(me->win) ? 4 : 0;
        if (me->win != nil && me->win->w_vsync_tick != me->vsync_seen) woke |= 2;
        return woke;
    }
    NSTimeInterval seconds = (NSTimeInterval)timeout_ms / 1000.0;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:deadline
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:NO];
    return event == nil ? 0 : 4;
}

void hmje_async_host_wake(void) {
    NSApplication *application = NSApp;
    if (application == nil) return;
    @autoreleasepool {
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:109
                                               data1:0
                                               data2:0];
        [application postEvent:event atStart:NO];
    }
}

/* ---- 앱을 연 주소(SYS_APP_OPEN_URL 120) --------------------------------------
 * 주 스레드에서만 쌓고 꺼낸다(AppKit 위임과 HMJE 루프가 모두 주 스레드). 순서대로 한 번씩
 * 꺼내므로 창이 몇 개든 주소 하나에 `URL이 열리면`은 한 번 돈다. */
static NSMutableArray<NSData *> *g_open_urls = nil;
/* 한 주소의 상한(UTF-8 바이트). 넘는 주소는 버린다 — 앱 루프가 한 번에 할당할 크기다. */
#define HMJE_OPEN_URL_MAX_BYTES (1u << 20)
/* 꺼내지 않은 채 쌓일 수 있는 주소 수. 넘치면 가장 오래된 것부터 버린다. */
#define HMJE_OPEN_URL_MAX_PENDING 256

static BOOL open_url_debug_enabled(void) {
    return getenv("HMJE_POSIX_DEBUG_OPEN_URL") != NULL || windows_debug_enabled();
}

static void app_open_url_enqueue(NSString *url) {
    NSData *bytes = [url dataUsingEncoding:NSUTF8StringEncoding];
    if (bytes == nil || [bytes length] == 0 || [bytes length] > HMJE_OPEN_URL_MAX_BYTES) return;
    if (g_open_urls == nil) g_open_urls = [NSMutableArray array];
    if ([g_open_urls count] >= HMJE_OPEN_URL_MAX_PENDING) [g_open_urls removeObjectAtIndex:0];
    [g_open_urls addObject:bytes];
    if (open_url_debug_enabled())
        fprintf(stderr, "posix-host-window: open-url queued (%lu pending)\n", (unsigned long)[g_open_urls count]);
    /* 창이 모두 닫혀 주 흐름이 RUN/FINISH에서 쉬고 있으면(계속 실행 정책) Dock을 누른 것처럼
     * 주 흐름의 창을 다시 연다 — 다시 연 창의 루프가 첫 프레임 뒤에 주소를 꺼낸다. 첫 창을
     * 만들기 전(차가운 시작)에는 주 흐름이 돌고 있으므로 다시 열기를 걸지 않는다. */
    if (g_window_events_aware && hmje_live_window_count() == 0 && g_fiber_main != nil
        && g_fiber_main->state == HMJE_FIBER_PARKED)
        g_reopen_requested = YES;
    /* 유휴 대기(앱 루프·창 흐름)를 깨운다. */
    hmje_async_host_wake();
}

/* a1=0: 다음 주소의 바이트 길이(없으면 0). a1=1: 다음 주소를 a2(용량 a3)에 복사하고
 * 큐에서 뺀다. 용량이 모자라면 복사하지 않고 -34(ERANGE)다. */
i64 hmje_posix_app_open_url(i64 operation, i64 address, i64 capacity) {
    if (![NSThread isMainThread]) return -22;
    NSData *head = [g_open_urls count] > 0 ? [g_open_urls objectAtIndex:0] : nil;
    if (operation == 0) return head == nil ? 0 : (i64)[head length];
    if (operation != 1) return -22;
    if (head == nil) return 0;
    if (address == 0 || capacity < (i64)[head length]) return -34;
    memcpy((void *)(uintptr_t)(u64)address, [head bytes], [head length]);
    i64 length = (i64)[head length];
    [g_open_urls removeObjectAtIndex:0];
    if (open_url_debug_enabled()) fprintf(stderr, "posix-host-window: open-url delivered\n");
    return length;
}

/* SYS_UI_DRAG_DROP uses the calling fiber's window queue. A short buffer does
 * not remove the event, so a UI loop can query length then allocate exactly. */
i64 hmje_posix_ui_drag_drop(i64 operation, i64 address, i64 capacity) {
    if (![NSThread isMainThread] || HMJE_WIN == nil) return -22;
    NSMutableArray<NSData *> *queue = HMJE_WIN->w_drop_events;
    NSData *head = [queue count] > 0 ? [queue objectAtIndex:0] : nil;
    if (operation == 0) return head == nil ? 0 : (i64)[head length];
    if (operation != 1) return -22;
    if (head == nil) return 0;
    if (address == 0 || capacity < (i64)[head length]) return -34;
    memcpy((void *)(uintptr_t)(u64)address, [head bytes], [head length]);
    i64 length = (i64)[head length];
    [queue removeObjectAtIndex:0];
    return length;
}

/* ---- 앱·창 생성 + 이벤트 펌프 ------------------------------------------- */

static void ensure_application(void) {
    if (g_app_ready) {
        return;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        HmjeApplicationDelegate *applicationDelegate = [[HmjeApplicationDelegate alloc] init];
        g_application_delegate = applicationDelegate; /* NSApplication.delegate는 weak다. */
        [NSApp setDelegate:applicationDelegate];

        /* Metal 디바이스를 창보다 먼저 준비한다 — makeBackingLayer가 참조한다. */
        g_device = MTLCreateSystemDefaultDevice();
        if (g_device != nil) {
            g_queue = [g_device newCommandQueue];
            if (g_queue == nil) {
                g_device = nil; /* 큐가 없으면 CGImage 폴백 */
            }
        }

        NSString *applicationName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"];
        if ([applicationName length] == 0) {
            applicationName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
        }
        if ([applicationName length] == 0) {
            applicationName = [[NSProcessInfo processInfo] processName];
        }
        install_standard_application_menu(applicationName);
        g_app_ready = YES;
        if (g_windows == nil) g_windows = [NSMutableArray array];
        hmje_fibers_init();

        /* run은 HMJE 루프와 충돌하므로 부르지 않지만, finishLaunching은
         * Dock Quit/Apple event/Cmd-Q가 표준 NSApplication 종료 경로로 오게 한다. */
        [NSApp finishLaunching];

        /* 키보드/IME 처리는 posix_host_text_input.m에 있다. KIND_KEY(+수정자) 큐잉과
         * NSTextInputClient 경유 KIND_TEXT(확정 문자) 큐잉을 그쪽이 전담한다.
         * 키 이벤트는 그 이벤트의 창으로 보낸다. */
        g_key_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged)
                                                                handler:^NSEvent *(NSEvent *event) {
            HmjeHostWindow *window = hmje_window_for_nswindow([event window]);
            if (window == nil) window = hmje_window_for_nswindow([NSApp keyWindow]);
            if (window == nil || window->w_closing) return event;
            HmjeHostWindow *previous = hmje_route_begin(window);
            NSEvent *result = hmje_posix_text_input_handle_event(event);
            hmje_route_end(previous);
            return result;
        }];

        g_pointer_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:
                                 (NSEventMaskMouseMoved
                                  | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged
                                  | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskRightMouseDragged
                                  | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskOtherMouseDragged
                                  | NSEventMaskScrollWheel | NSEventMaskTabletProximity)
                                                                handler:^NSEvent *(NSEvent *event) {
            return pointer_handle_event(event);
        }];
    }
}

/* 창 하나를 만든다. 크기는 논리 포인트. kind 2(보조)는 크기 고정·가운데. */
static HmjeHostWindow *create_window(u64 kind, u64 width, u64 height, BOOL positioned, i64 x, i64 y,
                                     NSString *title, BOOL primary) {
    ensure_application();
    HmjeHostWindow *host = [[HmjeHostWindow alloc] init];
    host->w_id = g_next_window_id++;
    host->w_kind = kind == 0 ? 1 : kind;
    host->w_primary = primary;
    host->w_scale = 1;
    host->w_gpu_target_needs_clear = YES;
    host->w_gpu_target_bg_needs_clear = YES;
    HmjeHostWindow *previous = hmje_route_begin(host);
    @autoreleasepool {
        if (width == 0) width = DEFAULT_WIDTH;
        if (height == 0) height = DEFAULT_HEIGHT;
        NSRect frame = NSMakeRect(120, 120, (CGFloat)width, (CGFloat)height);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (host->w_kind != 2) style |= NSWindowStyleMaskResizable;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                         styleMask:style
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
        if ([title length] == 0) {
            title = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"];
            if ([title length] == 0) title = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
            if ([title length] == 0) title = [[NSProcessInfo processInfo] processName];
        }
        [window setTitle:title];
        [window setReleasedWhenClosed:NO];
        [window setDelegate:g_application_delegate];
        if (primary) {
            /* AppKit이 번들 기본설정 도메인에 프레임을 저장한다. 첫 실행에만
             * 위 DEFAULT_*가 쓰이고, 다음 실행부터는 최근 크기·위치가 복원된다. */
            host->w_had_saved_frame = [window setFrameUsingName:@"HMJE.MainWindow"];
            [window setFrameAutosaveName:@"HMJE.MainWindow"];
        } else if (host->w_kind == 2) {
            [window center];
        } else if (positioned) {
            NSScreen *screen = [NSScreen mainScreen];
            CGFloat top = screen != nil ? NSMaxY([screen visibleFrame]) : 900;
            [window setFrameTopLeftPoint:NSMakePoint((CGFloat)x, top - (CGFloat)y)];
        } else {
            /* 문서 창은 앞 창에서 비껴 쌓는다(AppKit cascade). */
            static NSPoint cascade = {0, 0};
            if (cascade.x == 0 && cascade.y == 0) {
                [window center];
                cascade = NSMakePoint(NSMinX([window frame]), NSMaxY([window frame]));
            }
            cascade = [window cascadeTopLeftFromPoint:cascade];
        }
        if (host->w_kind != 2) [window setContentMinSize:NSMakeSize(MIN_CONTENT_WIDTH, MIN_CONTENT_HEIGHT)];
        if (host->w_kind == 2) [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenNone];

        HmjeContentView *view = [[HmjeContentView alloc] initWithFrame:NSMakeRect(0, 0, (CGFloat)width, (CGFloat)height)];
        view.hostWindow = host;
        [view registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypePNG,
                                        NSPasteboardTypeTIFF, NSPasteboardTypeString]];
        if (g_device != nil) {
            [view setWantsLayer:YES]; /* makeBackingLayer → CAMetalLayer */
        }
        [window setContentView:view];
        [window makeFirstResponder:view];

        host->w_window = window;
        host->w_view = view;
        [g_windows addObject:host];
        update_surface_geometry(NO); /* 초기 표면: 실제 창 크기 × 레티나 배율 */
        g_acked_width = g_width;
        g_acked_height = g_height;
        g_acked_scale = g_scale;
        g_ready = YES;
        surface_debug_log("init");

        hmje_posix_text_input_attach(window);
        /* 포인터. 버튼이 안 눌린 이동은 창이 명시적으로 요청해야 온다. */
        [window setAcceptsMouseMovedEvents:YES];
        if (windows_debug_enabled())
            fprintf(stderr, "posix-host-window: open id=%llu kind=%llu %llux%llu\n",
                    host->w_id, host->w_kind, width, height);
    }
    hmje_route_end(previous);
    return host;
}

static void show_window(HmjeHostWindow *host) {
    if (host == nil || host->w_shown || host->w_closing || host->w_window == nil) return;
    host->w_shown = YES;
    g_front_window = host;
    [host->w_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

/* 첫 창(주 흐름) — 처음 표면을 쓸 때 만든다. */
static void ensure_window(void) {
    if (g_win != nil && g_win->w_ready) {
        return;
    }
    if (g_fiber_current != nil && g_fiber_current != g_fiber_main) {
        return; /* 창 흐름의 창은 OPEN이 이미 만들었다(닫힌 뒤라면 다시 만들지 않는다). */
    }
    HmjeHostWindow *host = create_window(1, DEFAULT_WIDTH, DEFAULT_HEIGHT, NO, 0, 0, nil, YES);
    g_win = host;
    if (g_fiber_main != nil) g_fiber_main->win = host;
    @autoreleasepool {
        show_window(host);
        /* [NSApp run]을 안 부르므로, 창이 실제로 화면·키 포커스를 받으려면 초기 이벤트를
         * 몇 차례 수동으로 드레인해줘야 한다(윈도우 서버 왕복). */
        for (int i = 0; i < 20; i++) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                  untilDate:[NSDate distantPast]
                                                     inMode:NSDefaultRunLoopMode
                                                    dequeue:YES];
            if (event == nil) {
                break;
            }
            [NSApp sendEvent:event];
        }
        if (g_device == nil) {
            [g_view setNeedsDisplay:YES];
            [g_view displayIfNeeded];
        }
    }
}

#define HMJE_SELFTEST_TRACKPAD_SUBTYPE 110
static void selftest_trackpad_deliver(NSInteger milli_points, NSInteger phase, NSTimeInterval timestamp);

static void pump_events(void) {
    @autoreleasepool {
        for (int i = 0; i < 64; i++) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                  untilDate:[NSDate distantPast]
                                                     inMode:NSDefaultRunLoopMode
                                                    dequeue:YES];
            if (event == nil) {
                break;
            }
            if ([event type] == NSEventTypeApplicationDefined && [event subtype] == 109) {
                g_async_wake_count++;
            }
            if ([event type] == NSEventTypeApplicationDefined && [event subtype] == HMJE_VSYNC_WAKE_SUBTYPE) {
                continue; /* 주사 틱은 대기를 깨우기만 한다(HmjeVsyncTarget). */
            }
            if ([event type] == NSEventTypeApplicationDefined && [event subtype] == HMJE_SELFTEST_TRACKPAD_SUBTYPE) {
                selftest_trackpad_deliver([event data1], [event data2], [event timestamp]);
                continue;
            }
            [NSApp sendEvent:event];
        }
    }
}

/* 네이티브 창을 없앤다. 흐름이 끝난 뒤(FINISH)에만 부른다. */
static void destroy_window(HmjeHostWindow *host) {
    if (host == nil) return;
    HmjeHostWindow *previous = hmje_route_begin(host);
    @autoreleasepool {
        host->w_closing = YES;
        hmje_vsync_stop(host);
        NSWindow *window = host->w_window;
        if (window != nil) {
            [window setDelegate:nil];
            [window orderOut:nil];
            [window close];
        }
        host->w_ready = NO;
        host->w_layer = nil;
        host->w_staging = nil;
        host->w_gpu_target = nil;
        host->w_gpu_target_bg = nil;
        host->w_gpu_scroll_scratch = nil;
        if (g_device == nil && host->w_backbuffer != NULL) free(host->w_backbuffer);
        host->w_backbuffer = NULL;
        free(host->w_pointer_samples.grown_records);
        host->w_pointer_samples.grown_records = NULL;
        host->w_ax_elements = nil;
        host->w_ax_current = nil;
        host->w_text_state = nil;
        host->w_view = nil;
        host->w_window = nil;
        [g_windows removeObjectIdenticalTo:host];
        if (windows_debug_enabled())
            fprintf(stderr, "posix-host-window: closed id=%llu (live %llu)\n", host->w_id, hmje_live_window_count());
    }
    hmje_route_end(previous);
    if (g_win == host) g_win = nil;
}

static void request_application_quit(void) {
    g_quit_requested = YES;
    if (windows_debug_enabled()) fprintf(stderr, "posix-host-window: quit requested\n");
    for (HmjeHostWindow *window in g_windows) {
        if (!window->w_closing)
            window_push_event(window, HMJE_WINDOW_EVENT_CLOSE_REQUEST | HMJE_WINDOW_EVENT_QUIT_FLAG);
    }
}

/* 지금 흐름의 창 루프가 끝났다. 창을 없애고, 새 흐름이면 다른 흐름으로 넘어가 돌아오지
 * 않는다. 주 흐름이면 남은 창이 모두 닫힐 때까지 다른 흐름을 돌린 뒤 RUN 결과를 낸다. */
static i64 hmje_window_run_main(void);

static i64 hmje_window_finish(void) {
    hmje_fibers_init();
    HmjeFiber *me = g_fiber_current;
    HmjeHostWindow *host = me->win != nil ? me->win : g_win;
    if (host != nil) destroy_window(host);
    me->win = nil;
    g_win = nil;
    if (!me->is_main) {
        if (me->opener != nil) {
            /* STARTED 전에 끝났다 — OPEN에서 기다리는 여는 흐름을 풀어 준다. */
            me->opener->state = HMJE_FIBER_WAITING;
            me->opener->deadline = 0;
            me->opener = nil;
        }
        [g_fibers removeObjectIdenticalTo:me];
        [g_fiber_zombies addObject:me];
        hmje_fiber_block(HMJE_FIBER_DEAD, 0);
        abort(); /* 죽은 흐름은 다시 골라지지 않는다. */
    }
    return hmje_window_run_main();
}

static i64 hmje_window_run_main(void) {
    hmje_fibers_init();
    if (g_fiber_current != g_fiber_main) return -22;
    for (;;) {
        hmje_fiber_block(HMJE_FIBER_PARKED, 0);
        if (hmje_live_window_count() > 0) continue;
        if (g_quit_requested || g_last_window_policy == 1) {
            if (windows_debug_enabled()) fprintf(stderr, "posix-host-window: run -> quit\n");
            return 0;
        }
        if (g_reopen_requested) {
            g_reopen_requested = NO;
            if (windows_debug_enabled()) fprintf(stderr, "posix-host-window: run -> reopen\n");
            return 1;
        }
    }
}

static NSString *window_title_from(u64 address, u64 length) {
    if (length == 0 || address == 0 || length > (1u << 20)) return nil;
    return [[NSString alloc] initWithBytes:(const void *)(uintptr_t)address
                                    length:(NSUInteger)length
                                  encoding:NSUTF8StringEncoding];
}

/* SYS_UI_WINDOW(117). 연산은 운영체제 계약.hmje UI_WINDOW_OP_*. */
i64 hmje_posix_ui_window(i64 operation, i64 argument1, i64 argument2) {
    static NSMutableDictionary<NSNumber *, id> *activities;
    static u64 next_activity = 1;
    if (operation != 19 && operation != 20 && ![NSThread isMainThread]) return -22;
    switch (operation) {
    case 19: { /* APP_NAP_BEGIN: description UTF-8 bytes, returns activity token */
        if (argument1 == 0 || argument2 < 0) return -22;
        NSString *reason = [[NSString alloc] initWithBytes:(const void *)(uintptr_t)(u64)argument1
                                                    length:(NSUInteger)argument2 encoding:NSUTF8StringEncoding];
        if (reason == nil) return -22;
        id activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated
                                                                      reason:reason];
        if (activity == nil) return -12;
        @synchronized ([NSProcessInfo processInfo]) {
            if (activities == nil) activities = [NSMutableDictionary dictionary];
            u64 token = next_activity++;
            activities[@(token)] = activity;
            return (i64)token;
        }
    }
    case 20: { /* APP_NAP_END */
        @synchronized ([NSProcessInfo processInfo]) {
            NSNumber *key = @((u64)argument1);
            id activity = activities[key];
            if (activity == nil) return -2;
            [[NSProcessInfo processInfo] endActivity:activity];
            [activities removeObjectForKey:key];
            return 0;
        }
    }
    case 0: /* QUERY: bit0 = 호출자가 KIND_WINDOW 사건을 다룬다 */
        if (((u64)argument1 & 1ULL) != 0) {
            g_window_events_aware = YES;
            hmje_fibers_init();
        }
        return 1 | 2 | (g_quit_requested ? 4 : 0); /* MULTIPLE | KEEP_RUNNING_DEFAULT | QUITTING */
    case 1: { /* OPEN */
        if (argument1 == 0 || argument2 < 80) return -22;
        const u64 *descriptor = (const u64 *)(uintptr_t)(u64)argument1;
        if (descriptor[7] == 0) return -22;
        ensure_application();
        hmje_fibers_init();
        g_window_events_aware = YES;
        NSString *title = window_title_from(descriptor[5], descriptor[6]);
        BOOL positioned = (descriptor[9] & 1ULL) != 0;
        HmjeHostWindow *host = create_window(descriptor[0], descriptor[1], descriptor[2], positioned,
                                             (i64)descriptor[3], (i64)descriptor[4], title, NO);
        if (host == nil) return -12;
        HmjeFiber *fiber = hmje_fiber_create(host, descriptor[7], descriptor[8]);
        if (fiber == nil) {
            destroy_window(host);
            return -12;
        }
        [g_fibers addObject:fiber];
        /* 새 흐름으로 곧장 넘어간다. 진입 함수는 여는 쪽 스택의 진입 값(훅)을 자기
         * 스택으로 복사한 뒤 STARTED로 돌아온다 — 훅을 전역에 맡기지 않아도 된다.
         * 여는 흐름은 그동안 전역을 건드리지 않는다. */
        HmjeFiber *opener = g_fiber_current;
        fiber->opener = opener;
        opener->state = HMJE_FIBER_PARKED;
        hmje_fiber_switch_to(fiber);
        return (i64)host->w_id;
    }
    case 2: { /* CLOSE */
        HmjeHostWindow *host = argument1 == 0 ? g_win : hmje_window_for_id((u64)argument1);
        if (host == nil || host->w_window == nil) return -2;
        if (!host->w_closing) {
            hmje_persist_close(host->w_id, g_quit_requested);
            host->w_closing = YES;
            [host->w_window orderOut:nil];
            window_push_event(host, HMJE_WINDOW_EVENT_CLOSE);
        }
        return 0;
    }
    case 3: /* CURRENT */
        return g_win != nil ? (i64)g_win->w_id : 0;
    case 4: { /* POLICY */
        u64 previous = g_last_window_policy;
        if (argument1 == 1 || argument1 == 2) g_last_window_policy = (u64)argument1;
        return (i64)previous;
    }
    case 5: /* COUNT */
        return (i64)hmje_live_window_count();
    case 6: { /* ACTIVATE */
        HmjeHostWindow *host = hmje_window_for_id((u64)argument1);
        if (host == nil || host->w_closing) return -2;
        host->w_shown = YES;
        g_front_window = host;
        [host->w_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return 0;
    }
    case 13: { /* SET_TITLE: a2=window id, a3=pointer to {utf8 address, byte length} */
        HmjeHostWindow *host = hmje_window_for_id((u64)argument1);
        if (host == nil || host->w_closing || host->w_window == nil || argument2 == 0) return -22;
        const u64 *value = (const u64 *)(uintptr_t)(u64)argument2;
        if (value[1] > (1ULL << 20) || (value[1] != 0 && value[0] == 0)) return -22;
        /* HMJE가 같은 제목을 매 프레임 보낸다. AppKit 내부의 임시 객체를
         * 회수하고, 제목이 그대로면 WindowServer 갱신도 반복하지 않는다. */
        @autoreleasepool {
            NSString *title = window_title_from(value[0], value[1]);
            if (title == nil) title = @"";
            if (![[host->w_window title] isEqualToString:title]) {
                [host->w_window setTitle:title];
            }
        }
        return 0;
    }
    case 14: { /* APP_STATE */
        ensure_application();
        if ([NSApp isHidden] || hmje_live_window_count() == 0) return 3;
        BOOL visible = NO;
        for (HmjeHostWindow *host in g_windows) {
            if (!host->w_closing && host->w_window != nil &&
                ([host->w_window occlusionState] & NSWindowOcclusionStateVisible)) {
                visible = YES;
                break;
            }
        }
        if (!visible) return 3;
        return [NSApp isActive] ? 1 : 2;
    }
    case 15: { /* IS_OPEN */
        HmjeHostWindow *host = hmje_window_for_id((u64)argument1);
        return host != nil && host->w_window != nil && !host->w_closing ? 1 : 0;
    }
    case 21: { /* WINDOW_STATE */
        HmjeHostWindow *host = hmje_window_for_id((u64)argument1);
        return host != nil && host->w_window != nil && !host->w_closing &&
            host->w_key_active ? 1 : 2;
    }
    case 16: { /* FIT_CONTENT: root size modes + point dimensions, once before first present */
        if (g_win == nil || g_win->w_window == nil || argument1 == 0) return -22;
        const u64 *size = (const u64 *)(uintptr_t)(u64)argument1;
        if (size[0] > 2 || size[1] > 2) return -22;
        NSWindow *window = g_win->w_window;
        BOOL flexible = size[0] == 2 || size[1] == 2;
        NSWindowStyleMask style = [window styleMask];
        if (flexible) style |= NSWindowStyleMaskResizable;
        else style &= ~NSWindowStyleMaskResizable;
        [window setStyleMask:style];
        if (windows_debug_enabled())
            fprintf(stderr, "posix-host-window: fit id=%llu modes=%llu,%llu natural=%llux%llu resizable=%d\n",
                    (unsigned long long)g_win->w_id, (unsigned long long)size[0], (unsigned long long)size[1],
                    (unsigned long long)size[2], (unsigned long long)size[3],
                    ([window styleMask] & NSWindowStyleMaskResizable) != 0);
        if (g_win->w_had_saved_frame && flexible) return 0;
        NSScreen *screen = [window screen] ?: [NSScreen mainScreen];
        NSRect visible = screen != nil ? [screen visibleFrame] : NSMakeRect(0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT);
        NSSize content = [window contentRectForFrameRect:[window frame]].size;
        if (g_win->w_primary && size[0] == 2) content.width = visible.size.width - ([window frame].size.width - content.width);
        else if (size[0] != 2 && size[2] > 0) content.width = (CGFloat)size[2];
        if (g_win->w_primary && size[1] == 2) content.height = visible.size.height - ([window frame].size.height - content.height);
        else if (size[1] != 2 && size[3] > 0) content.height = (CGFloat)size[3];
        if (content.width < 1) content.width = 1;
        if (content.height < 1) content.height = 1;
        [window setContentSize:content];
        if (!flexible) {
            [window setContentMinSize:content];
            [window setContentMaxSize:content];
        }
        if (g_win->w_primary && !g_win->w_had_saved_frame) [window center];
        update_surface_geometry(YES);
        return 0;
    }
    case 17: { /* BIND: screen identity and value inputs */
        HmjeHostWindow *host = hmje_window_for_id((u64)argument1);
        if (host == nil || argument2 == 0) return -22;
        const u64 *data = (const u64 *)(uintptr_t)(u64)argument2;
        if (!data[0] || !data[1] || data[1] > HMJE_PERSIST_MAX_TEXT || data[3] > HMJE_PERSIST_MAX_TEXT ||
            (data[3] && !data[2])) return -22;
        const char *key = (const char *)(uintptr_t)data[0];
        hmje_persist_record *record = hmje_persist_bind(host->w_id, key, (size_t)data[1],
            (const char *)(uintptr_t)data[2], (size_t)data[3], data[4] != 0);
        if (!record) return -12;
        /* AppKit autosaves every additional window under its stable identity. */
        NSString *name = [NSString stringWithFormat:@"HMJE.Window.%016llx",
            (unsigned long long)hmje_persist_hash(key, (size_t)data[1])];
        host->w_had_saved_frame = [host->w_window setFrameUsingName:name];
        [host->w_window setFrameAutosaveName:name];
        if (windows_debug_enabled()) {
            NSRect frame = [host->w_window frame];
            fprintf(stderr, "posix-host-window: frame id=%llu restored=%d %.0f,%.0f %.0fx%.0f\n",
                    (unsigned long long)host->w_id, host->w_had_saved_frame,
                    frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
        }
        update_surface_geometry(YES);
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
        ensure_application();
        g_window_events_aware = YES;
        return hmje_window_run_main();
    case 9: /* QUIT_CANCEL */
        g_quit_requested = NO;
        if (windows_debug_enabled()) fprintf(stderr, "posix-host-window: quit cancelled\n");
        return 0;
    case 10: /* QUIT */
        request_application_quit();
        return 0;
    case 12: { /* STARTED */
        HmjeFiber *me = g_fiber_current;
        HmjeFiber *opener = me->opener;
        if (me->is_main || opener == nil) return -22;
        me->opener = nil;
        me->state = HMJE_FIBER_WAITING;
        me->deadline = 0; /* 여는 쪽이 다음에 기다리면 곧 돈다 */
        me->async_seen = g_async_wake_count;
        HmjeFiber *previous = g_fiber_current;
        g_fiber_current = opener;
        opener->state = HMJE_FIBER_RUNNING;
        g_win = opener->win;
        hmje_fiber_switch(&previous->sp, opener->sp);
        return 0;
    }
    case 11: { /* WAIT */
        hmje_fibers_init();
        if (argument1 < 0) return -22;
        HmjeFiber *me = g_fiber_current;
        BOOL previous = me->switchable;
        me->switchable = YES;
        int32_t flags = hmje_async_sleep_until_wake_ms((uint64_t)argument1);
        me->switchable = previous;
        return (i64)flags;
    }
    default:
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
}

/* ---- 셀프테스트 대상 창 ----------------------------------------------------
 * 셀프테스트 동작은 앞 창(마지막으로 키가 된 창)에 간다. `window:<ID>`가 앞 창을
 * 바꾼다. 앞 창이 닫히면 가장 최근에 보인 창으로 넘어간다. */
static HmjeHostWindow *selftest_target(void) {
    HmjeHostWindow *front = g_front_window;
    if (front != nil && !front->w_closing && front->w_window != nil) return front;
    for (HmjeHostWindow *window in [g_windows reverseObjectEnumerator]) {
        if (!window->w_closing && window->w_shown) return window;
    }
    for (HmjeHostWindow *window in [g_windows reverseObjectEnumerator]) {
        if (!window->w_closing) return window;
    }
    return nil;
}

/* ---- 셀프테스트: 창 크기를 프로그램적으로 바꿔 리사이즈 경로를 자동 검증 ---- */
/* 카운터는 "빈 입력 poll"(프레임당 정확히 1번 — 앱 드레인의 종료 poll)로 센다.
 * present는 리드로가 있는 프레임에만 오므로(idle 최적화) 카운터가 멈출 수 있다. */

static void maybe_selftest_resize(void) {
    static BOOL done = NO;
    static u64 frame_count = 0;
    if (done) {
        return;
    }
    const char *spec = getenv("HMJE_POSIX_SELFTEST_RESIZE");
    if (spec == NULL) {
        done = YES;
        return;
    }
    frame_count++;
    unsigned long long target_w = 0, target_h = 0, at_frame = 90;
    if (sscanf(spec, "%llux%llu@%llu", &target_w, &target_h, &at_frame) < 2) {
        if (sscanf(spec, "%llux%llu", &target_w, &target_h) < 2) {
            fprintf(stderr, "posix-host-surface: selftest spec parse failed: %s\n", spec);
            done = YES;
            return;
        }
        at_frame = 90;
    }
    if (frame_count < at_frame) {
        return;
    }
    done = YES;
    fprintf(stderr, "posix-host-surface: selftest resizing window content to %llux%llu points\n",
            target_w, target_h);
    [g_window setContentSize:NSMakeSize((CGFloat)target_w, (CGFloat)target_h)];
    /* setContentSize → contentView setFrameSize → update_surface_geometry가
     * SURFACE_RESIZE 이벤트를 큐잉한다. 혹시 AppKit이 뷰 리사이즈를 지연시켜도
     * 아래 직접 호출이 잡는다(기하가 같으면 no-op이라 이중 이벤트는 없다). */
    update_surface_geometry(YES);
}

/* ---- 셀프테스트: 포인터를 합성해 상호작용 경로를 자동 검증 ----------------
 * HMJE_POSIX_SELFTEST_POINTER="move:250,250;press:0;move:550,450;release:0;scroll:0,-120"
 *   좌표는 **표면 픽셀**(왼쪽 위 원점) — 계약과 같은 단위다. 레티나 창이면
 *   논리 좌표의 두 배라는 뜻이고, 앱이 보는 값은 앱 루프가 논리로 낮춘 값이다.
 *   `wait:N`은 그 동작 전에 N 프레임을 흘려 보낸다(앱이 재구축·표시할 틈).
 *   HMJE_POSIX_SELFTEST_POINTER_AT=프레임 으로 시작 시점을 늦출 수 있다(기본 60).
 *   `resize:WxH`는 창 내용 크기(포인트)를 바꾸고, `close`는 창 닫기(APP_CLOSE)를 보낸다.
 *   글자 입력: `ime:한`(조합 표시) · `commit:한`(확정) · `key:1,c`(⌘S, macOS keyCode와
 *   수식자 c/s/o/t) — posix_host_text_input.m의 hmje_posix_text_input_selftest_action.
 *   `trackpad:속도,손가락ms[,Hz]`는 실제 시계로 도는 트랙패드 흐름(손가락 + 관성)을,
 *   `waitms:N`은 벽시계 N ms 기다림을 뜻한다(selftest_trackpad_start 주석).
 *
 * NSEvent를 실제로 만들어 우리 핸들러에 태우므로 좌표 변환·인코딩·앱 반응까지
 * 다 지난다. **안 지나는 것 하나**: AppKit이 창에 이벤트를 배달하는 구간이다
 * (그건 사람이 마우스를 움직여야 나오고, CGEvent 합성은 TCC 권한을 요구한다).
 * 사람이 하는 확인을 대신하지는 못하고, 회귀를 잡는 게 목적이다. */

static NSEvent *synth_mouse_event(NSEventType type, u64 px, u64 py, NSInteger button) {
    if (g_view == nil || g_window == nil || g_scale == 0) {
        return nil;
    }
    /* 뷰가 flipped라 여기도 뒤집지 않는다 — 위 pointer_pixel_location의 역이다. */
    CGFloat scale_f = (CGFloat)g_scale;
    NSPoint in_view = NSMakePoint((CGFloat)px / scale_f, (CGFloat)py / scale_f);
    NSPoint in_window = [g_view convertPoint:in_view toView:nil];
    return [NSEvent mouseEventWithType:type
                              location:in_window
                         modifierFlags:0
                             timestamp:[[NSProcessInfo processInfo] systemUptime]
                          windowNumber:[g_window windowNumber]
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:(type == NSEventTypeLeftMouseDown ? 1.0 : 0.0)];
}

/* 프레임 덤프 번호 매기기(dump_frame_path 주석). 셀프테스트 포인터가 도는 동안은
 * present가 아니라 **포인터 동작**이 번호를 올린다 — 두 실행이 같은 번호에서 같은
 * 포인터 위치를 뜻하게 하려면 그 방법뿐이다. */
static u64 g_dump_present_index = 0;
static u64 g_selftest_step_index = 0;
static BOOL g_selftest_pointer_active = NO;

/* 셀프테스트 지연 계측(HMJE_POSIX_SELFTEST_LATENCY=1, 성능 벤치 전용).
 * 합성 동작을 넣은 시각부터 그 뒤 첫 화면 present가 GPU에서 끝난 시각까지를
 * `posix-host-latency:`로 찍는다 — 키 입력에서 화면에 보인 프레임까지의 값이다.
 * 같은 변수가 켜져 있으면 모든 화면 present 시각도 `posix-host-present:`로 남겨
 * 스크롤 중 표시 간격을 잴 수 있게 한다. 끄면 getenv 한 번 외에 비용이 없다. */
static int g_latency_mode = -1;
static u64 g_latency_start_us = 0;
static u64 g_latency_step = 0;
static char g_latency_action[64];

static BOOL latency_enabled(void) {
    if (g_latency_mode < 0) {
        const char *value = getenv("HMJE_POSIX_SELFTEST_LATENCY");
        g_latency_mode = (value != NULL && value[0] == '1') ? 1 : 0;
        if (g_latency_mode == 1) {
            NSInteger hz = [NSScreen mainScreen] != nil ? [[NSScreen mainScreen] maximumFramesPerSecond] : 0;
            fprintf(stderr, "posix-host-display: hz=%ld\n", (long)hz);
        }
    }
    return g_latency_mode == 1;
}

static u64 latency_now_us(void) {
    return (u64)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u);
}

static void latency_mark_action(u64 step, const char *action) {
    if (!latency_enabled() || strncmp(action, "wait:", 5) == 0) {
        return;
    }
    g_latency_start_us = latency_now_us();
    g_latency_step = step;
    snprintf(g_latency_action, sizeof(g_latency_action), "%s", action);
}

/* GPU 완료 처리기(다른 스레드)로 넘기는 지연 계측 한 건. */
typedef struct {
    int armed;
    u64 start_us;
    u64 step;
    char action[64];
} latency_capture;

static latency_capture latency_take_pending(void) {
    latency_capture capture;
    memset(&capture, 0, sizeof capture);
    if (!latency_enabled()) return capture;
    capture.armed = 1;
    capture.start_us = g_latency_start_us;
    capture.step = g_latency_step;
    memcpy(capture.action, g_latency_action, sizeof capture.action);
    g_latency_start_us = 0;
    return capture;
}

static void latency_report(latency_capture capture) {
    if (!capture.armed) return;
    u64 now = latency_now_us();
    fprintf(stderr, "posix-host-present: t=%llu\n", (unsigned long long)now);
    if (capture.start_us != 0) {
        fprintf(stderr, "posix-host-latency: step=%llu us=%llu action=%s\n",
                (unsigned long long)capture.step,
                (unsigned long long)(now - capture.start_us), capture.action);
    }
}

static void latency_mark_present(void) {
    if (!latency_enabled()) {
        return;
    }
    u64 now = latency_now_us();
    fprintf(stderr, "posix-host-present: t=%llu\n", (unsigned long long)now);
    if (g_latency_start_us != 0) {
        fprintf(stderr, "posix-host-latency: step=%llu us=%llu action=%s\n",
                (unsigned long long)g_latency_step,
                (unsigned long long)(now - g_latency_start_us), g_latency_action);
        g_latency_start_us = 0;
    }
}

static void selftest_push_scroll(u64 x, u64 y, i64 delta_x, i64 delta_y) {
    hmje_pointer_record record;
    memset(&record, 0, sizeof(record));
    record.phase = HMJE_POINTER_PHASE_SCROLL;
    record.pointer_id = 1u;
    record.x = (i64)x;
    record.y = (i64)y;
    record.time_us = (u64)([[NSProcessInfo processInfo] systemUptime] * 1000000.0);
    record.scroll_x = delta_x;
    record.scroll_y = delta_y;
    record.tool = HMJE_POINTER_TOOL_MOUSE;
    record.flags = HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL;
    hmje_pointer_ring_push(&g_pointer_samples, record);

    key_ring_push(pointer_event_base(HMJE_INPUT_KIND_POINTER_MOVE, x, y));
    i64 legacy_y = delta_y;
    if (legacy_y != 0) {
        BOOL down = legacy_y < 0;
        u64 amount = (u64)(down ? -legacy_y : legacy_y);
        if (amount > HMJE_INPUT_SCROLL_AMOUNT_MASK) amount = HMJE_INPUT_SCROLL_AMOUNT_MASK;
        u64 packed = (HMJE_INPUT_KIND_POINTER_SCROLL << HMJE_INPUT_KIND_SHIFT)
                   | (amount & HMJE_INPUT_SCROLL_AMOUNT_MASK);
        if (down) packed |= HMJE_INPUT_SCROLL_DOWN_FLAG;
        key_ring_push(packed);
    }
}

/* ---- 셀프테스트: 트랙패드 흐름 ------------------------------------------------
 * `trackpad:속도,손가락ms[,Hz]` — 손가락이 트랙패드를 `속도`(포인트/초, 음수 = 문서 아래로)로
 * `손가락ms` 동안 쓸고 떼면 AppKit처럼 관성(모멘텀) 표본이 지수 감속(시정수 350ms)으로
 * 이어진다. 표본은 **앱 루프와 무관한 별도 스레드**가 실제 시계로 Hz(기본 120)마다 만든다 —
 * 앱이 바쁘면 실제 기기처럼 큐에 쌓이고, 쉬고 있으면 표본마다 깨운다. 그래서 프레임마다 동작
 * 하나를 넣는 다른 셀프테스트 동작과 달리 표시 페이싱·병합·누락 프레임을 잴 수 있다.
 * 표본은 응용 정의 사건(부표 110)으로 AppKit 큐를 지나 pump_events가 앞 창의 정밀 스크롤
 * 경로(pointer_precise_scroll_take)에 넣는다. 지연 계측이 켜져 있으면 표본마다
 * `posix-host-trackpad:`를 남긴다. `waitms:N`은 다음 동작을 벽시계 N ms 뒤로 미룬다. */
static u64 g_selftest_cursor_x = 0, g_selftest_cursor_y = 0;

typedef struct {
    double velocity;
    double finger_ms;
    double hz;
} hmje_trackpad_spec;

enum {
    HMJE_TRACKPAD_FINGER_BEGAN = 1,
    HMJE_TRACKPAD_FINGER_CHANGED = 2,
    HMJE_TRACKPAD_FINGER_ENDED = 3,
    HMJE_TRACKPAD_MOMENTUM_BEGAN = 4,
    HMJE_TRACKPAD_MOMENTUM_CHANGED = 5,
    HMJE_TRACKPAD_MOMENTUM_ENDED = 6,
};

static u64 trackpad_now_ns(void) {
    return (u64)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void trackpad_sleep_until(u64 target_ns) {
    for (;;) {
        u64 now = trackpad_now_ns();
        if (now >= target_ns) return;
        u64 left = target_ns - now;
        struct timespec request = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
        nanosleep(&request, NULL);
    }
}

static void trackpad_post(double delta_points, int phase) {
    @autoreleasepool {
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:[[NSProcessInfo processInfo] systemUptime]
                                        windowNumber:0
                                             context:nil
                                             subtype:HMJE_SELFTEST_TRACKPAD_SUBTYPE
                                               data1:(NSInteger)llround(delta_points * 1000.0)
                                               data2:phase];
        /* postEvent:atStart:는 다른 스레드에서도 부를 수 있다(AppKit 문서). */
        [NSApp postEvent:event atStart:NO];
    }
}

static void *trackpad_thread(void *argument) {
    hmje_trackpad_spec spec = *(hmje_trackpad_spec *)argument;
    free(argument);
    double hz = spec.hz > 0.0 ? spec.hz : 120.0;
    u64 period = (u64)(1000000000.0 / hz);
    u64 next = trackpad_now_ns();
    trackpad_post(0.0, HMJE_TRACKPAD_FINGER_BEGAN);
    u64 finger_end = next + (u64)(spec.finger_ms * 1000000.0);
    double step = spec.velocity / hz;
    for (;;) {
        next += period;
        if (next > finger_end) break;
        trackpad_sleep_until(next);
        trackpad_post(step, HMJE_TRACKPAD_FINGER_CHANGED);
    }
    trackpad_sleep_until(next);
    trackpad_post(0.0, HMJE_TRACKPAD_FINGER_ENDED);
    /* 관성: v(t) = v0·e^(−t/350ms), 20pt/s 아래로 떨어지면 끝. */
    double elapsed = 0.0;
    int phase = HMJE_TRACKPAD_MOMENTUM_BEGAN;
    for (;;) {
        next += period;
        elapsed += 1.0 / hz;
        double velocity = spec.velocity * exp(-elapsed / 0.35);
        if (fabs(velocity) < 20.0) break;
        trackpad_sleep_until(next);
        trackpad_post(velocity / hz, phase);
        phase = HMJE_TRACKPAD_MOMENTUM_CHANGED;
    }
    next += period;
    trackpad_sleep_until(next);
    trackpad_post(0.0, HMJE_TRACKPAD_MOMENTUM_ENDED);
    return NULL;
}

static void selftest_trackpad_start(double velocity, double finger_ms, double hz) {
    hmje_trackpad_spec *spec = (hmje_trackpad_spec *)malloc(sizeof *spec);
    if (spec == NULL) return;
    spec->velocity = velocity;
    spec->finger_ms = finger_ms;
    spec->hz = hz;
    pthread_t thread;
    if (pthread_create(&thread, NULL, trackpad_thread, spec) != 0) {
        free(spec);
        return;
    }
    pthread_detach(thread);
}

static void selftest_trackpad_deliver(NSInteger milli_points, NSInteger phase, NSTimeInterval timestamp) {
    HmjeHostWindow *target = selftest_target();
    if (target == nil) return;
    if (latency_enabled()) {
        fprintf(stderr, "posix-host-trackpad: t=%llu phase=%ld dy=%ld\n",
                (unsigned long long)latency_now_us(), (long)phase, (long)milli_points);
    }
    HmjeHostWindow *previous = hmje_route_begin(target);
    i64 pixel_x = 0, pixel_y = 0;
    if (pointer_precise_scroll_take(0.0, (double)milli_points / 1000.0, &pixel_x, &pixel_y)) {
        hmje_pointer_record record;
        memset(&record, 0, sizeof(record));
        record.phase = HMJE_POINTER_PHASE_SCROLL;
        record.pointer_id = 1u;
        record.x = (i64)g_selftest_cursor_x;
        record.y = (i64)g_selftest_cursor_y;
        record.time_us = (u64)(timestamp * 1000000.0);
        record.scroll_x = pixel_x;
        record.scroll_y = pixel_y;
        record.tool = HMJE_POINTER_TOOL_MOUSE;
        record.flags = HMJE_POINTER_CAP_BUTTONS | HMJE_POINTER_CAP_SCROLL;
        hmje_pointer_ring_push(&g_pointer_samples, record);
    }
    hmje_route_end(previous);
}

static void maybe_selftest_pointer(void) {
    static BOOL done = NO;
    static u64 frame_count = 0;
    static char *script = NULL;
    static char *step = NULL;
    static u64 wait_until = 0;
    static u64 wait_until_us = 0;
    if (done) {
        return;
    }
    const char *spec = getenv("HMJE_POSIX_SELFTEST_POINTER");
    if (spec == NULL) {
        done = YES;
        return;
    }
    frame_count++;
    g_selftest_pointer_active = YES;
    if (script == NULL) {
        const char *at = getenv("HMJE_POSIX_SELFTEST_POINTER_AT");
        wait_until = at != NULL ? strtoull(at, NULL, 10) : 60;
        script = strdup(spec);
        step = script;
    }
    if (frame_count < wait_until) {
        return;
    }
    if (wait_until_us != 0) {
        if (latency_now_us() < wait_until_us) return;
        wait_until_us = 0;
    }
    /* 프레임당 한 동작. 한 프레임에 몰아 넣으면 앱이 중간 상태를 못 보고,
     * 끌기는 이동 이벤트 사이의 재구축이 있어야 실제 경로를 밟는다. */
    if (step == NULL || *step == '\0') {
        done = YES;
        g_selftest_pointer_active = NO;
        if (g_dump_present_index <= g_selftest_step_index) {
            g_dump_present_index = g_selftest_step_index + 1;
        }
        fprintf(stderr, "posix-host-pointer: selftest done\n");
        return;
    }
    char *end = strchr(step, ';');
    if (end != NULL) {
        *end = '\0';
    }
    char *action = step;
    step = (end != NULL) ? end + 1 : NULL;
    /* 동작 하나를 소비했다 = 덤프 번호 하나. 이 동작이 낳는 present가 몇 번이든
     * 같은 파일을 덮어써서 **정착된** 프레임이 남는다. */
    g_selftest_step_index++;

    unsigned long long a = 0, b = 0;
    long long signed_a = 0, signed_b = 0;
    if (sscanf(action, "move:%llu,%llu", &a, &b) == 2) {
        g_selftest_cursor_x = a;
        g_selftest_cursor_y = b;
        NSEvent *event = synth_mouse_event(NSEventTypeMouseMoved, g_selftest_cursor_x, g_selftest_cursor_y, 0);
        if (event != nil) {
            pointer_handle_event(event);
        }
    } else if (sscanf(action, "drag:%llu,%llu", &a, &b) == 2) {
        g_selftest_cursor_x = a;
        g_selftest_cursor_y = b;
        NSEvent *event = synth_mouse_event(NSEventTypeLeftMouseDragged, g_selftest_cursor_x, g_selftest_cursor_y, 0);
        if (event != nil) {
            pointer_handle_event(event);
        }
    } else if (sscanf(action, "press:%llu", &a) == 1) {
        NSEvent *event = synth_mouse_event(NSEventTypeLeftMouseDown, g_selftest_cursor_x, g_selftest_cursor_y, (NSInteger)a);
        if (event != nil) {
            pointer_handle_event(event);
        }
    } else if (sscanf(action, "release:%llu", &a) == 1) {
        NSEvent *event = synth_mouse_event(NSEventTypeLeftMouseUp, g_selftest_cursor_x, g_selftest_cursor_y, (NSInteger)a);
        if (event != nil) {
            pointer_handle_event(event);
        }
    } else if (sscanf(action, "scroll:%lld,%lld", &signed_a, &signed_b) == 2) {
        selftest_push_scroll(g_selftest_cursor_x, g_selftest_cursor_y, (i64)signed_a, (i64)signed_b);
    } else if (sscanf(action, "scroll:%lld", &signed_b) == 1) {
        selftest_push_scroll(g_selftest_cursor_x, g_selftest_cursor_y, 0, (i64)signed_b);
    } else if (sscanf(action, "wheel:%lld,%lld", &signed_a, &signed_b) == 2) {
        /* 노치 휠(AppKit 부호의 가로·세로 노치 수) — 실제 마우스 휠과 같은 pointer_push_notches를
         * 지난다. Shift+휠은 AppKit이 바꿔 준 가로 노치(wheel:-3,0)다. */
        pointer_push_notches(g_selftest_cursor_x, g_selftest_cursor_y,
                             (u64)([[NSProcessInfo processInfo] systemUptime] * 1000000.0),
                             signed_a, signed_b);
    } else if (strcmp(action, "close") == 0) {
        /* 닫기 단추를 누른 것과 같다(창 메뉴 "창 닫기"와도 같다). */
        if (g_window != nil) [g_window performClose:nil];
    } else if (sscanf(action, "window:%llu", &a) == 1) {
        /* 다음 동작부터 창 a를 앞 창으로. 프레임도 그 창의 poll로 센다. */
        HmjeHostWindow *target = hmje_window_for_id(a);
        if (target != nil && !target->w_closing) {
            g_front_window = target;
            [target->w_window makeKeyAndOrderFront:nil];
        }
    } else if (strcmp(action, "quit") == 0) {
        /* ⌘Q(앱 메뉴 "종료")와 같은 AppKit 종료 경로. */
        [NSApp terminate:nil];
    } else if (sscanf(action, "resize:%llux%llu", &a, &b) == 2) {
        [g_window setContentSize:NSMakeSize((CGFloat)a, (CGFloat)b)];
        update_surface_geometry(YES);
    } else if (sscanf(action, "wait:%llu", &a) == 1) {
        wait_until = frame_count + a;
    } else if (sscanf(action, "waitms:%llu", &a) == 1) {
        wait_until_us = latency_now_us() + a * 1000ull;
    } else if (strncmp(action, "trackpad:", 9) == 0) {
        double velocity = 0.0, finger_ms = 0.0, hz = 120.0;
        if (sscanf(action + 9, "%lf,%lf,%lf", &velocity, &finger_ms, &hz) >= 2) {
            selftest_trackpad_start(velocity, finger_ms, hz);
        }
    } else if (hmje_posix_text_input_selftest_action(action)) {
        /* ime:/commit:/key: — posix_host_text_input.m */
    } else {
        fprintf(stderr, "posix-host-pointer: 모르는 동작 '%s'\n", action);
    }
    fprintf(stderr, "posix-host-pointer: %s (커서 %llu,%llu)\n", action, g_selftest_cursor_x, g_selftest_cursor_y);
    latency_mark_action(g_selftest_step_index, action);
}

/* ---- 훈민정음 GPU 해석기가 부르는 평평한 Metal ABI ----------------------- */

#define HMJE_GPU_BEGIN 1
#define HMJE_GPU_CLIP 2
#define HMJE_GPU_UPLOAD_GLYPH 3
#define HMJE_GPU_DRAW 4
#define HMJE_GPU_IMAGE_BLIT 5
#define HMJE_GPU_END_PRESENT 6
#define HMJE_GPU_ABORT 7
#define HMJE_GPU_SCROLL_SHIFT 8
#define HMJE_GPU_COMPOSITE_TARGET 9
#define HMJE_GPU_IMAGE_SCALE_BLIT 12
#define HMJE_GPU_LAYER_BEGIN 13
#define HMJE_GPU_LAYER_END 14

typedef struct {
    float origin[2];
    float size[2];
    float color[4];
    float viewport[2];
    float padding[2];
    float params[4];
    float shadow[4];
} HmjeGpuUniforms;

static const char *hmje_gpu_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct U{float2 origin;float2 size;float4 color;float2 viewport;float2 padding;float4 params;float4 shadow;};\n"
"struct O{float4 position [[position]];};\n"
"float sd(float2 l,float2 s,float r){float2 h=s*.5;float2 q=abs(l-h)-h+r;return length(max(q,float2(0)))+min(max(q.x,q.y),0.)-r;}\n"
"uint cv(float2 l,float2 s,float r){return uint(clamp(.5-sd(l,s,r),0.,1.)*255.);}\n"
"float af(uint c,float a){uint x=uint(round(a*255.));return float((c*x)/255u)/255.;}\n"
"vertex O vs(uint i [[vertex_id]],constant U&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
"fragment float4 ps(O i [[stage_in]],constant U&u [[buffer(0)]]){int k=int(u.params.x+.5);if(k==0)return u.color;float2 l=i.position.xy-u.origin;if(k==3){float2 a=u.shadow.xy,b=u.shadow.zw,v=b-a;float vv=dot(v,v);float t=vv>1e-6?clamp(dot(i.position.xy-a,v)/vv,0.,1.):0.;float d=length(i.position.xy-(a+t*v));float f=clamp(u.params.y+.5-d,0.,1.);return float4(u.color.rgb,af(uint(f*255.),u.color.a));}if(k==5){float b=u.params.z;float2 so=u.origin+float2(b);float2 ss=u.size-float2(2.*b);float d=sd(i.position.xy-so,ss,u.params.y);float f;if(b<=0.)f=clamp(.5-d,0.,1.);else{float t=clamp((d+b)/(2.*b),0.,1.);f=1.-t*t*(3.-2.*t);}float inner=clamp(.5-sd(i.position.xy-u.shadow.xy,u.shadow.zw,u.params.w),0.,1.);f*=1.-inner;return float4(u.color.rgb,af(uint(f*255.),u.color.a));}uint o=cv(l,u.size,u.params.y);uint c=o;if(k==2){float2 s=u.size-float2(u.params.w*2.);uint n=cv(l-float2(u.params.w),s,u.params.z);c=o>n?o-n:0u;}return float4(u.color.rgb,af(c,u.color.a));}\n"
"fragment float4 pg(O i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float,access::read> g [[texture(0)]]){int2 c=int2(floor((i.position.xy-u.origin)/max(u.params.y,1.)));uint v=uint(round(g.read(uint2(c)).r*255.));return float4(u.color.rgb,af(v,u.color.a));}\n"
"struct OI{float4 position [[position]];float2 uv;};\n"
"vertex OI vi(uint i [[vertex_id]],constant U&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;OI o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);o.uv=q;return o;}\n"
"fragment float4 pi(OI i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float> src [[texture(0)]]){constexpr sampler samp(coord::normalized,address::clamp_to_edge,filter::nearest);float4 c=src.sample(samp,i.uv);uint cov=cv(i.position.xy-u.origin,u.size,u.params.y);float a=u.params.z>0.5?af(cov,c.a):float(cov)/255.;return float4(c.rgb,a);}\n"
/* 층 합성: 그룹 불투명도. 층 텍스처는 시작 시 대상 픽셀을 그대로 떠서
 * 만들었으므로(HMJE_GPU_LAYER_BEGIN) 알파 채널 없이도 결과가 정확하다 —
 * CPU 재생기(command_interpreter.hmje)와 같은 모델이다. */
"fragment float4 pl(OI i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float> src [[texture(0)]]){constexpr sampler samp(coord::normalized,address::clamp_to_edge,filter::nearest);float4 c=src.sample(samp,i.uv);return float4(c.rgb,u.color.a);}\n";

static BOOL hmje_gpu_prepare_pipelines(void) {
    if (g_gpu_shape_pipeline != nil && g_gpu_glyph_pipeline != nil && g_gpu_image_pipeline != nil &&
        g_gpu_layer_pipeline != nil) return YES;
    NSError *error = nil;
    id<MTLLibrary> library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:hmje_gpu_msl]
                                                    options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "posix-host-gpu: shader compile failed: %s\n", error.localizedDescription.UTF8String);
        return NO;
    }
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [library newFunctionWithName:@"vs"];
    description.fragmentFunction = [library newFunctionWithName:@"ps"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    description.colorAttachments[0].blendingEnabled = YES;
    description.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    description.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    description.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    description.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    description.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    description.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
    g_gpu_shape_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_gpu_shape_pipeline == nil) return NO;
    description.fragmentFunction = [library newFunctionWithName:@"pg"];
    g_gpu_glyph_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_gpu_glyph_pipeline == nil) return NO;
    description.vertexFunction = [library newFunctionWithName:@"vi"];
    description.fragmentFunction = [library newFunctionWithName:@"pi"];
    g_gpu_image_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_gpu_image_pipeline == nil) return NO;
    description.fragmentFunction = [library newFunctionWithName:@"pl"];
    g_gpu_layer_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    return g_gpu_layer_pipeline != nil;
}

static void hmje_gpu_end_render_encoder(void) {
    if (g_gpu_render_encoder != nil) {
        [g_gpu_render_encoder endEncoding];
        g_gpu_render_encoder = nil;
    }
}

static id<MTLRenderCommandEncoder> hmje_gpu_encoder(void) {
    if (g_gpu_render_encoder != nil) return g_gpu_render_encoder;
    id<MTLTexture> target = hmje_gpu_active_target();
    BOOL *needsClear = hmje_gpu_active_needs_clear_flag();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = *needsClear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_gpu_render_encoder = [g_gpu_command_buffer renderCommandEncoderWithDescriptor:pass];
    *needsClear = NO;
    [g_gpu_render_encoder setScissorRect:g_gpu_clip];
    return g_gpu_render_encoder;
}

/* BEGIN 요청은 [너비, 높이] 16바이트(target=0 기본값, 과거 호환) 또는
 * [너비, 높이, target] 24바이트를 받는다. target 0=화면(캔버스값, 매 프레임
 * drawable에 present), target 1=배경(persistent, 앱이 직접 present하지 않고
 * 캔버스값 쪽 합성 소스로만 쓰인다 — GPU_COMMAND_COMPOSITE_TARGET 참고). */
static i64 hmje_gpu_begin(const u64 *request, u64 length) {
    if ((length != 16 && length != 24) || request == NULL) return HMJE_POSIX_ENOSYS_LOCAL;
    const char *enabled = getenv("HMJE_POSIX_GPU");
    if (enabled != NULL && strcmp(enabled, "0") == 0) return HMJE_POSIX_ENOSYS_LOCAL;
    ensure_window();
    u64 width = request[0], height = request[1];
    int target = (length == 24 && request[2] == 1) ? 1 : 0;
    if (g_device == nil || width == 0 || height == 0 || !hmje_gpu_prepare_pipelines())
        return HMJE_POSIX_ENOSYS_LOCAL;
    g_gpu_current_target = target;
    __strong id<MTLTexture> *slot = target == 1 ? &g_gpu_target_bg : &g_gpu_target;
    BOOL *needsClear = target == 1 ? &g_gpu_target_bg_needs_clear : &g_gpu_target_needs_clear;
    /* 앞선 트랜잭션이 GPU에서 실패했다(END는 기다리지 않고 성공을 돌려줬다). 이
     * 대상의 실패 하나를 소비하고 이번 BEGIN을 거절한다 — 공통 렌더러가 이 대상의
     * 동기화 플래그를 내리고 CPU 화면으로 복구한 뒤, 다음 시도는 전체 재동기화다. */
    unsigned failedBit = target == 1 ? 2u : 1u;
    unsigned priorFailures = atomic_fetch_and_explicit(&HMJE_WIN->w_gpu_async_failed, ~failedBit,
                                                       memory_order_acq_rel);
    if ((priorFailures & failedBit) != 0) {
        *needsClear = YES;
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (*slot == nil || (u64)(*slot).width != width || (u64)(*slot).height != height) {
        id<MTLTexture> previous = *slot;
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
        description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        *slot = [g_device newTextureWithDescriptor:description];
        *needsClear = YES;
        /* 리사이즈로 target을 다시 만들 때 이전 내용을 옮겨 심는다. 검정으로
         * 비우고 부분 프레임을 기다리면 전체 리페인트가 도착하기 전까지
         * 안 그려진 영역이 검은 띠로 남을 수 있다. 이전 내용의 배율은
         * 한 프레임 어긋날 수 있지만 리사이즈 경로의 전체 리페인트가 곧바로 덮는다. */


        if (previous != nil && *slot != nil) {
            id<MTLCommandBuffer> copy_commands = [g_queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [copy_commands blitCommandEncoder];
            NSUInteger copy_w = MIN((NSUInteger)previous.width, (NSUInteger)width);
            NSUInteger copy_h = MIN((NSUInteger)previous.height, (NSUInteger)height);
            [blit copyFromTexture:previous sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                        toTexture:*slot destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [copy_commands commit];
            /* 같은 큐라 뒤 렌더 패스와 순서가 보장된다 — 대기 불필요. 복사해
             * 넣었으니 load-clear로 지우면 안 된다. */
            *needsClear = NO;
        }
    }
    if (*slot == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    g_gpu_command_buffer = [g_queue commandBuffer];
    g_gpu_render_encoder = nil;
    g_gpu_clip = (MTLScissorRect){0, 0, (NSUInteger)width, (NSUInteger)height};
    return g_gpu_command_buffer != nil ? 1 : HMJE_POSIX_ENOSYS_LOCAL;
}

static i64 hmje_gpu_set_clip(const u64 *r, u64 length) {
    if (length != 32 || r == NULL || g_gpu_command_buffer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    id<MTLTexture> target = hmje_gpu_active_target();
    u64 left=r[0], top=r[1], right=r[2], bottom=r[3];
    if (left > (u64)target.width) left=(u64)target.width;
    if (top > (u64)target.height) top=(u64)target.height;
    if (right < left) right=left;
    if (bottom < top) bottom=top;
    if (right > (u64)target.width) right=(u64)target.width;
    if (bottom > (u64)target.height) bottom=(u64)target.height;
    g_gpu_clip = (MTLScissorRect){(NSUInteger)left,(NSUInteger)top,
        (NSUInteger)(right-left),(NSUInteger)(bottom-top)};
    if (g_gpu_render_encoder != nil && g_gpu_clip.width > 0 && g_gpu_clip.height > 0)
        [g_gpu_render_encoder setScissorRect:g_gpu_clip];
    return 1;
}

#include "metal_glyph_cache.inc"
static i64 hmje_gpu_upload_glyph(const u64 *r, u64 length) {
    if (length != 32 || r == NULL) return HMJE_POSIX_ENOSYS_LOCAL;
    g_gpu_glyph = hmje_metal_glyph_texture(g_device,
        (const unsigned char *)(uintptr_t)r[0], r[1], r[2], r[3]);
    return g_gpu_glyph != nil ? 1 : HMJE_POSIX_ENOSYS_LOCAL;
}

static i64 hmje_gpu_draw(const u64 *request, u64 length) {
    if (length != 104 || request == NULL || g_gpu_command_buffer == nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    int kind = (int)request[0];
    if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4 && kind != 5) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (g_gpu_clip.width == 0 || g_gpu_clip.height == 0) return 1;
    if (kind != 3 && (request[3] == 0 || request[4] == 0)) return 1;
    if (kind == 3 && request[7] == 0) return 1;
    if (kind == 4 && g_gpu_glyph == nil) return HMJE_POSIX_ENOSYS_LOCAL;

    id<MTLTexture> target = hmje_gpu_active_target();
    if (target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    HmjeGpuUniforms uniforms = {0};
    if (kind == 3) {
        /* HMJE has already flattened the cubic.  Draw one conservative quad
         * around this round-cap segment; the fragment shader evaluates exact
         * capsule distance, including the zero-length (circle) case. */
        double ax = (double)(i64)request[1], ay = (double)(i64)request[2];
        double bx = (double)(i64)request[3], by = (double)(i64)request[4];
        if (request[7] > UINT64_C(16777216)) return HMJE_POSIX_ENOSYS_LOCAL;
        double radius = (double)request[7] * 0.5;
        double left = floor(fmin(ax, bx) - radius - 1.0);
        double top = floor(fmin(ay, by) - radius - 1.0);
        double right = ceil(fmax(ax, bx) + radius + 1.0);
        double bottom = ceil(fmax(ay, by) + radius + 1.0);
        double clip_left = (double)g_gpu_clip.x;
        double clip_top = (double)g_gpu_clip.y;
        double clip_right = clip_left + (double)g_gpu_clip.width;
        double clip_bottom = clip_top + (double)g_gpu_clip.height;
        left = fmax(left, clip_left);
        top = fmax(top, clip_top);
        right = fmin(right, clip_right);
        bottom = fmin(bottom, clip_bottom);
        if (right <= left || bottom <= top) return 1;
        uniforms.origin[0] = (float)left;
        uniforms.origin[1] = (float)top;
        uniforms.size[0] = (float)(right - left);
        uniforms.size[1] = (float)(bottom - top);
        uniforms.params[1] = (float)radius;
        uniforms.shadow[0] = (float)ax;
        uniforms.shadow[1] = (float)ay;
        uniforms.shadow[2] = (float)bx;
        uniforms.shadow[3] = (float)by;
    } else {
        uniforms.origin[0] = (float)(i64)request[1];
        uniforms.origin[1] = (float)(i64)request[2];
        uniforms.size[0] = (float)request[3];
        uniforms.size[1] = (float)request[4];
        uniforms.params[1] = (float)request[5];
        uniforms.shadow[0] = (float)(i64)request[9];
        uniforms.shadow[1] = (float)(i64)request[10];
        uniforms.shadow[2] = (float)request[11];
        uniforms.shadow[3] = (float)request[12];
    }
    uniforms.color[0] = (float)((request[8] >> 16) & 255) / 255.0f;
    uniforms.color[1] = (float)((request[8] >> 8) & 255) / 255.0f;
    uniforms.color[2] = (float)(request[8] & 255) / 255.0f;
    uniforms.color[3] = (float)((request[8] >> 24) & 255) / 255.0f;
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    uniforms.params[0] = (float)kind;
    uniforms.params[2] = (float)request[6];
    uniforms.params[3] = (float)request[7];
    if (kind == 1 || kind == 2) {
        u64 outer = MIN(request[5], MIN(request[3] / 2, request[4] / 2));
        uniforms.params[1] = (float)outer;
        if (kind == 2) {
            u64 innerWidth = request[7] <= request[3] / 2 ? request[3] - 2 * request[7] : 0;
            u64 innerHeight = request[7] <= request[4] / 2 ? request[4] - 2 * request[7] : 0;
            uniforms.params[2] = (float)MIN(request[6], MIN(innerWidth / 2, innerHeight / 2));
        }
    }

    id<MTLRenderCommandEncoder> encoder = hmje_gpu_encoder();
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder setRenderPipelineState:kind == 4
        ? g_gpu_glyph_pipeline : g_gpu_shape_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    if (kind == 4) [encoder setFragmentTexture:g_gpu_glyph atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    return 1;
}

static u64 hmje_macos_gpu_negative_magnitude(i64 value) {
    return (u64)(-(value + 1)) + 1;
}

/* Crop a 1:1 copy against source bounds, signed destination bounds, and the
 * current clip while advancing the source origin by the same amount. */
static BOOL hmje_macos_gpu_crop_copy(u64 source_width, u64 source_height,
                                   u64 target_width, u64 target_height,
                                   MTLScissorRect clip,
                                   u64 *source_x, u64 *source_y,
                                   u64 *copy_width, u64 *copy_height,
                                   i64 *destination_x, i64 *destination_y) {
    if (*source_x >= source_width || *source_y >= source_height ||
        *copy_width == 0 || *copy_height == 0 || clip.width == 0 || clip.height == 0) {
        return NO;
    }
    if (*copy_width > source_width - *source_x) *copy_width = source_width - *source_x;
    if (*copy_height > source_height - *source_y) *copy_height = source_height - *source_y;

    if (*destination_x < 0) {
        u64 amount = hmje_macos_gpu_negative_magnitude(*destination_x);
        if (amount >= *copy_width) return NO;
        *source_x += amount;
        *copy_width -= amount;
        *destination_x = 0;
    }
    if (*destination_y < 0) {
        u64 amount = hmje_macos_gpu_negative_magnitude(*destination_y);
        if (amount >= *copy_height) return NO;
        *source_y += amount;
        *copy_height -= amount;
        *destination_y = 0;
    }

    u64 dx = (u64)*destination_x, dy = (u64)*destination_y;
    if (dx >= target_width || dy >= target_height) return NO;
    if (*copy_width > target_width - dx) *copy_width = target_width - dx;
    if (*copy_height > target_height - dy) *copy_height = target_height - dy;

    u64 clip_left = (u64)clip.x, clip_top = (u64)clip.y;
    u64 clip_right = clip_left + (u64)clip.width;
    u64 clip_bottom = clip_top + (u64)clip.height;
    if (dx < clip_left) {
        u64 amount = clip_left - dx;
        if (amount >= *copy_width) return NO;
        *source_x += amount;
        *copy_width -= amount;
        dx = clip_left;
        *destination_x = (i64)dx;
    }
    if (dy < clip_top) {
        u64 amount = clip_top - dy;
        if (amount >= *copy_height) return NO;
        *source_y += amount;
        *copy_height -= amount;
        dy = clip_top;
        *destination_y = (i64)dy;
    }
    if (dx >= clip_right || dy >= clip_bottom) return NO;
    if (*copy_width > clip_right - dx) *copy_width = clip_right - dx;
    if (*copy_height > clip_bottom - dy) *copy_height = clip_bottom - dy;
    return *copy_width > 0 && *copy_height > 0;
}

static i64 hmje_gpu_image_blit(const u64 *r, u64 length) {
    if (length != 80 || r == NULL || g_gpu_command_buffer == nil || r[0] == 0) return HMJE_POSIX_ENOSYS_LOCAL;
    id<MTLTexture> target = hmje_gpu_active_target();
    u64 sourceWidth=r[1], sourceHeight=r[2], sourcePitch=r[3];
    if (sourceWidth > UINT64_MAX / 4 || sourcePitch < sourceWidth * 4 ||
        (sourceHeight != 0 && sourcePitch > UINT64_MAX / sourceHeight))
        return HMJE_POSIX_ENOSYS_LOCAL;
    u64 sourceX=r[4], sourceY=r[5], copyWidth=r[6], copyHeight=r[7];
    i64 destinationX=(i64)r[8], destinationY=(i64)r[9];
    if (!hmje_macos_gpu_crop_copy(sourceWidth, sourceHeight, target.width, target.height,
        g_gpu_clip, &sourceX, &sourceY, &copyWidth, &copyHeight, &destinationX, &destinationY))
        return 1;
    if (copyWidth > NSUIntegerMax / 4 || copyHeight > NSUIntegerMax / (copyWidth * 4))
        return HMJE_POSIX_ENOSYS_LOCAL;
    NSUInteger rowBytes=(NSUInteger)(copyWidth*4), byteCount=rowBytes*(NSUInteger)copyHeight;
    id<MTLBuffer> upload=[g_device newBufferWithLength:byteCount options:MTLResourceStorageModeShared];
    if (upload == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    unsigned char *destination=(unsigned char *)upload.contents;
    const unsigned char *source=(const unsigned char *)(uintptr_t)r[0]+sourceY*sourcePitch+sourceX*4;
    for (u64 row=0; row<copyHeight; ++row)
        memcpy(destination+row*rowBytes, source+row*sourcePitch, rowBytes);
    hmje_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder=[g_gpu_command_buffer blitCommandEncoder];
    [encoder copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:byteCount
                 sourceSize:MTLSizeMake((NSUInteger)copyWidth,(NSUInteger)copyHeight,1)
                  toTexture:target destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake((NSUInteger)destinationX,(NSUInteger)destinationY,0)];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

/* GPU_COMMAND_IMAGE_SCALE_BLIT: 이미지 위젯(늘리기/여백맞춤/채우기). 위
 * hmje_gpu_image_blit과 달리 대상 사각형이 원본 영역과 다른 크기일 수 있어
 * 단순 텍스처-투-텍스처/버퍼-투-텍스처 blit(크기 불변 전제)으로는 안 된다 —
 * 대신 원본 크롭을 소스 텍스처로 올리고, 텍스처 샘플러(nearest, hmje-ui
 * core.hmje 캔버스이미지스케일그리기의 CPU 최근접 이웃 기준과 같은 사양)로
 * 대상 사각형 쿼드를 그린다. 소스 텍스처는 replaceRegion의 bytesPerRow가
 * 이미지 피치를 그대로 받아들이므로, 기존 hmje_gpu_image_blit처럼 별도
 * 스테이징 버퍼로 행을 다시 다져 넣을 필요가 없다. */
static i64 hmje_gpu_image_scale_blit(const u64 *r, u64 length) {
    if ((length != 104 && length != 112) || r == NULL || (length == 112 && r[13] != 1) || g_gpu_command_buffer == nil || r[0] == 0) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_gpu_clip.width == 0 || g_gpu_clip.height == 0) return 1;
    u64 sourceWidth=r[1], sourceHeight=r[2], sourcePitch=r[3];
    u64 sourceX=r[4], sourceY=r[5], copyWidth=r[6], copyHeight=r[7];
    i64 destinationX=(i64)r[8], destinationY=(i64)r[9];
    u64 destinationWidth=r[10], destinationHeight=r[11];
    if (sourceWidth > UINT64_MAX / 4 || sourcePitch < sourceWidth * 4 ||
        (sourceHeight != 0 && sourcePitch > UINT64_MAX / sourceHeight))
        return HMJE_POSIX_ENOSYS_LOCAL;
    if (sourceX >= sourceWidth || sourceY >= sourceHeight || destinationWidth == 0 || destinationHeight == 0)
        return 1;
    if (copyWidth > sourceWidth-sourceX) copyWidth=sourceWidth-sourceX;
    if (copyHeight > sourceHeight-sourceY) copyHeight=sourceHeight-sourceY;
    if (copyWidth == 0 || copyHeight == 0) return 1;
    /* Command buffers execute later. A subsequent image of the same size must
     * not replace pixels referenced by an already encoded draw. Metal retains
     * this immutable per-command upload until the command buffer completes. */
    MTLTextureDescriptor *description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:(NSUInteger)copyWidth height:(NSUInteger)copyHeight mipmapped:NO];
    description.usage = MTLTextureUsageShaderRead;
    description.storageMode = MTLStorageModeShared;
    id<MTLTexture> imageSource = [g_device newTextureWithDescriptor:description];
    if (imageSource == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    const unsigned char *source=(const unsigned char *)(uintptr_t)r[0]+sourceY*sourcePitch+sourceX*4;
    [imageSource replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)copyWidth,(NSUInteger)copyHeight)
                                mipmapLevel:0 withBytes:source bytesPerRow:(NSUInteger)sourcePitch];
    HmjeGpuUniforms u = {0};
    u.origin[0]=(float)destinationX; u.origin[1]=(float)destinationY;
    u.size[0]=(float)destinationWidth; u.size[1]=(float)destinationHeight;
    u64 imageRadius=r[12];
    if (imageRadius > destinationWidth/2) imageRadius=destinationWidth/2;
    if (imageRadius > destinationHeight/2) imageRadius=destinationHeight/2;
    u.params[1]=(float)imageRadius;
    u.params[2]=length == 112 ? 1.f : 0.f; /* explicit straight RGBA; legacy XRGB stays opaque */
    id<MTLTexture> activeTarget = hmje_gpu_active_target();
    u.viewport[0]=(float)activeTarget.width; u.viewport[1]=(float)activeTarget.height;
    id<MTLRenderCommandEncoder> encoder = hmje_gpu_encoder();
    [encoder setRenderPipelineState:g_gpu_image_pipeline];
    [encoder setVertexBytes:&u length:sizeof(u) atIndex:0];
    [encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [encoder setFragmentTexture:imageSource atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    return 1;
}

/* GPU_COMMAND_COMPOSITE_TARGET: 다른 persistent target(현재는 항상 배경,
 * source_target=1)의 GPU 텍스처를 현재 활성 target(현재는 항상 화면, target=0)
 * 으로 텍스처-투-텍스처 blit한다. gpu_command_renderer.hmje가 이번 프레임
 * 화면 명령버퍼의 이미지블릿이 실제로는 "배경 자신의 GPU 텍스처를 그대로
 * 복사하라"는 뜻임을 감지했을 때만 이 op을 낸다(주소가 배경 캔버스의 CPU
 * 버퍼 기준주소와 일치하고 배경 GPU 텍스처가 최신 상태일 때) — 그 밖의
 * 임의 CPU 픽셀(아이콘/글리프/진단 버퍼) 이미지블릿은 여전히 기존
 * hmje_gpu_image_blit(CPU 업로드) 경로를 쓴다.
 * 페이로드(56B): [source_target, srcX, srcY, copyW, copyH, dstX, dstY]. */
static i64 hmje_gpu_composite_target(const u64 *r, u64 length) {
    if (length != 56 || r == NULL || g_gpu_command_buffer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    /* 원본 대상이 GPU에서 실패했으면(비트는 그 대상의 BEGIN이 소비한다) 합성을
     * 거절해 공통 렌더러가 원본 동기화를 내리게 한다. */
    unsigned sourceFailedBit = r[0] == 1 ? 2u : 1u;
    if ((atomic_load_explicit(&HMJE_WIN->w_gpu_async_failed, memory_order_acquire) & sourceFailedBit) != 0)
        return HMJE_POSIX_ENOSYS_LOCAL;
    id<MTLTexture> source = r[0] == 1 ? g_gpu_target_bg : g_gpu_target;
    id<MTLTexture> destination = hmje_gpu_active_target();
    if (source == nil || destination == nil || source == destination) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 sourceX=r[1], sourceY=r[2], copyWidth=r[3], copyHeight=r[4], destinationX=r[5], destinationY=r[6];
    if (sourceX >= (u64)source.width || sourceY >= (u64)source.height ||
        destinationX >= (u64)destination.width || destinationY >= (u64)destination.height) return 1;
    if (copyWidth > (u64)source.width-sourceX) copyWidth=(u64)source.width-sourceX;
    if (copyHeight > (u64)source.height-sourceY) copyHeight=(u64)source.height-sourceY;
    if (copyWidth > (u64)destination.width-destinationX) copyWidth=(u64)destination.width-destinationX;
    if (copyHeight > (u64)destination.height-destinationY) copyHeight=(u64)destination.height-destinationY;
    if (copyWidth == 0 || copyHeight == 0) return 1;
    hmje_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder=[g_gpu_command_buffer blitCommandEncoder];
    [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake((NSUInteger)sourceX,(NSUInteger)sourceY,0)
                   sourceSize:MTLSizeMake((NSUInteger)copyWidth,(NSUInteger)copyHeight,1)
                    toTexture:destination destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake((NSUInteger)destinationX,(NSUInteger)destinationY,0)];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

/* library/runtime/user/draw_commands.hmje의 명령태그_세로스크롤시프트 계약과
 * 동일한 CPU 시맨틱(hmje-ui/oslib/graphics/core.hmje 캔버스세로이동)을 GPU
 * 텍스처 안에서 재현한다: 영역(x,y,w,h) 안에서 이동량만큼 행을 위/아래로
 * 밀고, 드러난 스트립은 그대로 둔다(같은 명령 버퍼의 다음 명령들이 그 스트립을
 * 전부 덮어 그린다는 게 호출자 쪽 불변식 — command_interpreter.hmje의
 * CPU 재생과 동일).
 * 같은 텍스처 안에서 겹치는 영역을 copyFromTexture:toTexture:로 직접 옮기는
 * 것은 Metal에서 정의되지 않은 동작이라(source/destination이 aliasing),
 * 임시 스크래치 텍스처로 한 번 옮겼다가 다시 옮기는 2단계 blit을 쓴다. */
static i64 hmje_gpu_scroll_shift(const u64 *r, u64 length) {
    id<MTLTexture> activeTarget = hmje_gpu_active_target();
    if (length != 48 || r == NULL || g_gpu_command_buffer == nil || activeTarget == nil)
        return HMJE_POSIX_ENOSYS_LOCAL;
    i64 sx = (i64)r[0], sy = (i64)r[1];
    u64 w = r[2], h = r[3], shift = r[4];
    BOOL down = r[5] != 0;
    u64 targetWidth = (u64)activeTarget.width, targetHeight = (u64)activeTarget.height;
    if (sx < 0) {
        u64 clip = (u64)(-sx);
        if (clip >= w) return 1;
        w -= clip; sx = 0;
    }
    if (sy < 0) {
        u64 clip = (u64)(-sy);
        if (clip >= h) return 1;
        h -= clip; sy = 0;
    }
    u64 x = (u64)sx, y = (u64)sy;
    if (x >= targetWidth || y >= targetHeight || w == 0 || h == 0) return 1;
    if (w > targetWidth - x) w = targetWidth - x;
    if (h > targetHeight - y) h = targetHeight - y;
    if (shift == 0 || shift >= h) return 1; /* CPU 쪽과 동일: 겹치는 픽셀 없음 → no-op */
    u64 copyHeight = h - shift;
    u64 srcY = down ? y : y + shift;
    u64 dstY = down ? y + shift : y;

    u64 scratchWidth = w, scratchHeight = copyHeight;
    if (g_gpu_scroll_scratch != nil) {
        scratchWidth = MAX(scratchWidth, (u64)g_gpu_scroll_scratch.width);
        scratchHeight = MAX(scratchHeight, (u64)g_gpu_scroll_scratch.height);
    }
    if (g_gpu_scroll_scratch == nil || (u64)g_gpu_scroll_scratch.width < w ||
        (u64)g_gpu_scroll_scratch.height < copyHeight) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                          width:(NSUInteger)scratchWidth
                                         height:(NSUInteger)scratchHeight
                                      mipmapped:NO];
        description.usage = MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        g_gpu_scroll_scratch = [g_device newTextureWithDescriptor:description];
    }
    if (g_gpu_scroll_scratch == nil) return HMJE_POSIX_ENOSYS_LOCAL;

    hmje_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_gpu_command_buffer blitCommandEncoder];
    [encoder copyFromTexture:activeTarget sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)srcY, 0)
                   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)copyHeight, 1)
                    toTexture:g_gpu_scroll_scratch destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder copyFromTexture:g_gpu_scroll_scratch sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)copyHeight, 1)
                    toTexture:activeTarget destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)dstY, 0)];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

/* GPU_COMMAND_PLANAR_SHIFT: SCROLL_SHIFT의 두 축 판(캔버스 팬). 같은 텍스처 안에서
 * 겹치는 blit이라 스크래치를 경유하는 것도, 스크래치를 grow-only로 재사용하는 것도
 * 8번과 같다. 페이로드(64B): [x, y, w, h, dx, dy, right, down] — dx/dy는 크기이고
 * 방향은 뒤 두 칸이다(CPU 쪽 명령_평면시프트와 같은 표현).
 *
 * 클램프는 CPU(캔버스평면이동)와 **같은 순서**여야 한다: 영역을 대상 안으로 자른 뒤
 * |델타|가 변 이상이면 겹치는 픽셀이 없어 no-op. 순서가 어긋나면 GPU와 CPU가 다른
 * 픽셀을 옮겨 parity가 깨진다. */
#define HMJE_GPU_PLANAR_SHIFT 15

static i64 hmje_gpu_planar_shift(const u64 *r, u64 length) {
    id<MTLTexture> activeTarget = hmje_gpu_active_target();
    if (length != 64 || r == NULL || g_gpu_command_buffer == nil || activeTarget == nil)
        return HMJE_POSIX_ENOSYS_LOCAL;
    i64 sx = (i64)r[0], sy = (i64)r[1];
    u64 w = r[2], h = r[3], dx = r[4], dy = r[5];
    BOOL right = r[6] != 0, down = r[7] != 0;
    u64 targetWidth = (u64)activeTarget.width, targetHeight = (u64)activeTarget.height;
    if (sx < 0) {
        u64 clip = (u64)(-sx);
        if (clip >= w) return 1;
        w -= clip; sx = 0;
    }
    if (sy < 0) {
        u64 clip = (u64)(-sy);
        if (clip >= h) return 1;
        h -= clip; sy = 0;
    }
    u64 x = (u64)sx, y = (u64)sy;
    if (x >= targetWidth || y >= targetHeight || w == 0 || h == 0) return 1;
    if (w > targetWidth - x) w = targetWidth - x;
    if (h > targetHeight - y) h = targetHeight - y;
    if ((dx == 0 && dy == 0) || dx >= w || dy >= h) return 1;
    u64 copyWidth = w - dx, copyHeight = h - dy;
    u64 srcX = right ? x : x + dx;
    u64 dstX = right ? x + dx : x;
    u64 srcY = down ? y : y + dy;
    u64 dstY = down ? y + dy : y;

    u64 scratchWidth = copyWidth, scratchHeight = copyHeight;
    if (g_gpu_scroll_scratch != nil) {
        scratchWidth = MAX(scratchWidth, (u64)g_gpu_scroll_scratch.width);
        scratchHeight = MAX(scratchHeight, (u64)g_gpu_scroll_scratch.height);
    }
    if (g_gpu_scroll_scratch == nil || (u64)g_gpu_scroll_scratch.width < copyWidth ||
        (u64)g_gpu_scroll_scratch.height < copyHeight) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                          width:(NSUInteger)scratchWidth
                                         height:(NSUInteger)scratchHeight
                                      mipmapped:NO];
        description.usage = MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        g_gpu_scroll_scratch = [g_device newTextureWithDescriptor:description];
    }
    if (g_gpu_scroll_scratch == nil) return HMJE_POSIX_ENOSYS_LOCAL;

    hmje_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_gpu_command_buffer blitCommandEncoder];
    [encoder copyFromTexture:activeTarget sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake((NSUInteger)srcX, (NSUInteger)srcY, 0)
                   sourceSize:MTLSizeMake((NSUInteger)copyWidth, (NSUInteger)copyHeight, 1)
                    toTexture:g_gpu_scroll_scratch destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder copyFromTexture:g_gpu_scroll_scratch sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)copyWidth, (NSUInteger)copyHeight, 1)
                    toTexture:activeTarget destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake((NSUInteger)dstX, (NSUInteger)dstY, 0)];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

#define HMJE_METAL_BLUR_MARK_INITIALIZED() (*hmje_gpu_active_needs_clear_flag() = NO)
#include "metal_backdrop_blur.inc"
#undef HMJE_METAL_BLUR_MARK_INITIALIZED

#define HMJE_GPU_GRADIENT_FILL 11

/* GPU_COMMAND_GRADIENT_FILL: 균등 간격 정지점 선형 그라데이션 채우기.
 * CPU 기준(core.hmje 캔버스그라데이션채우기)의 사양을 프래그먼트 1개당 순수
 * 보간으로 재현하는 단일 패스다:
 *   축 위치 = 픽셀 정수 인덱스(논리 영역 원점 기준, floor(중심좌표) - 원점),
 *   t = p / 분모(세로 h-1 / 가로 w-1 / 대각 (w-1)+(h-1)),
 *   s = t·(색상수-1), 구간 = floor(s)(끝은 색상수-2), 조각 = s-구간,
 *   채널 = round(c0 + (c1-c0)·조각) — CPU가 실수64로 1회 반올림하는 것과
 *   대수적으로 같은 식(0..255 스케일에서 반올림 후 정규화),
 *   rounded-box SDF 하드 엣지 커버리지×보간 알파를 (cov·a)/255 정수 절삭으로
 *   합쳐 src 알파로 고정기능 블렌드(도형 파이프라인과 같은 합성 계약).
 * 페이로드(120B): [x, y, w, h, cornerRadius, direction, count, c1..c8]. */
static const char *hmje_gpu_gradient_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct GU{float2 origin;float2 size;float2 viewport;float2 misc;float4 params;float4 colors[8];};\n"
"struct O{float4 position [[position]];};\n"
"float gsd(float2 l,float2 s,float r){float2 h=s*.5;float2 q=abs(l-h)-h+r;return length(max(q,float2(0)))+min(max(q.x,q.y),0.)-r;}\n"
"vertex O gvs(uint i [[vertex_id]],constant GU&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
"fragment float4 gfs(O i [[stage_in]],constant GU&u [[buffer(0)]]){\n"
"float2 lc=i.position.xy-u.origin;\n"
"float px=floor(lc.x);float py=floor(lc.y);\n"
"int dir=int(u.misc.x+.5);int n=int(u.misc.y+.5);\n"
"float w=u.size.x;float h=u.size.y;\n"
"float p=dir==1?py:(dir==3?(px+py):px);\n"
"float denom=dir==1?(h-1.):(dir==3?((w-1.)+(h-1.)):(w-1.));\n"
"float t=denom>0.?clamp(p/denom,0.,1.):0.;\n"
"if(dir==4||dir==5){float2 rr=max((u.size-1.)*.5,float2(.5));if(dir==4)rr=float2(min(rr.x,rr.y));t=length((float2(px,py)-(u.size-1.)*.5)/rr);}\n"
"float s=t*float(n-1);\n"
"int seg=clamp(int(floor(s)),0,n-2);\n"
"float frac=s-float(seg);\n"
"float4 c0=u.colors[seg];float4 c1=u.colors[seg+1];\n"
"float4 c=round(c0+(c1-c0)*frac);\n"
"uint covb=uint(saturate(.5-gsd(lc,u.size,u.params.x))*255.);\n"
"uint fillA=uint(c.a);\n"
"uint combined=(covb*fillA)/255u;\n"
"return float4(c.rgb/255.,float(combined)/255.);}\n";

typedef struct {
    float origin[2];
    float size[2];
    float viewport[2];
    float misc[2];      /* x=방향(1/2/3), y=색상수 */
    float params[4];    /* x=모서리반경(클램프 후) */
    float colors[8][4]; /* 0..255 스케일 RGBA — 프래그먼트가 round 후 정규화 */
} HmjeGpuGradientUniforms;

static BOOL hmje_gpu_prepare_gradient_pipeline(void) {
    if (g_gpu_gradient_pipeline != nil) return YES;
    NSError *error = nil;
    id<MTLLibrary> library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:hmje_gpu_gradient_msl]
                                                    options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "posix-host-gpu: gradient shader compile failed: %s\n",
                error.localizedDescription.UTF8String);
        return NO;
    }
    /* 도형 파이프라인과 같은 고정기능 블렌드(BGRA target). */
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [library newFunctionWithName:@"gvs"];
    description.fragmentFunction = [library newFunctionWithName:@"gfs"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    description.colorAttachments[0].blendingEnabled = YES;
    description.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    description.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    description.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    description.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    description.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    description.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
    g_gpu_gradient_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    return g_gpu_gradient_pipeline != nil;
}

static i64 hmje_gpu_gradient_fill(const u64 *r, u64 length) {
    id<MTLTexture> target = hmje_gpu_active_target();
    if (length != 120 || r == NULL || g_gpu_command_buffer == nil || target == nil)
        return HMJE_POSIX_ENOSYS_LOCAL;
    if (!hmje_gpu_prepare_gradient_pipeline()) return HMJE_POSIX_ENOSYS_LOCAL;
    i64 regionX = (i64)r[0], regionY = (i64)r[1];
    u64 regionWidth = r[2], regionHeight = r[3];
    u64 cornerRadius = r[4], direction = r[5], count = r[6];
    if (regionWidth == 0 || regionHeight == 0) return 1;
    if (count < 2) return 1; /* CPU와 동일: 정지점 부족은 조용한 no-op */
    if (count > 8) count = 8;
    /* 모서리 반경 클램프(CPU와 동일 규칙). */
    u64 radius = cornerRadius;
    if (radius > regionWidth / 2) radius = regionWidth / 2;
    if (radius > regionHeight / 2) radius = regionHeight / 2;

    HmjeGpuGradientUniforms u = {{0}};
    u.origin[0] = (float)regionX; u.origin[1] = (float)regionY;
    u.size[0] = (float)regionWidth; u.size[1] = (float)regionHeight;
    u.viewport[0] = (float)target.width; u.viewport[1] = (float)target.height;
    u.misc[0] = (float)direction; u.misc[1] = (float)count;
    u.params[0] = (float)radius;
    for (u64 i = 0; i < 8; ++i) {
        uint32_t argb = (uint32_t)r[7 + (i < count ? i : count - 1)];
        u.colors[i][0] = (float)((argb >> 16) & 255);
        u.colors[i][1] = (float)((argb >> 8) & 255);
        u.colors[i][2] = (float)(argb & 255);
        u.colors[i][3] = (float)((argb >> 24) & 255);
    }

    /* 도형과 같은 target에 그리지만 셰이더/유니폼이 달라 별도 패스로 그린다 —
     * 공유 lazy encoder를 닫고 LoadActionLoad로 전용 패스를 연다(블러 합성
     * 패스와 같은 구조, 현재 scissor 존중). */
    hmje_gpu_end_render_encoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [g_gpu_command_buffer renderCommandEncoderWithDescriptor:pass];
    if (g_gpu_clip.width > 0 && g_gpu_clip.height > 0)
        [encoder setScissorRect:g_gpu_clip];
    [encoder setRenderPipelineState:g_gpu_gradient_pipeline];
    [encoder setVertexBytes:&u length:sizeof(u) atIndex:0];
    [encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

static void dump_gpu_target_debug(void);
static void dump_gpu_target_bg_debug(void);

/* target 0(화면)은 기존과 동일하게 drawable로 blit→present→commit한다.
 * target 1(배경)은 화면에 아무것도 보여주지 않는 persistent 저장소이므로
 * drawable을 취하지 않고 커맨드 버퍼만 commit해 GPU에 실제로 반영시킨다 —
 * 그래야 다음에 이 텍스처를 소스로 쓰는 COMPOSITE_TARGET/다음 프레임 배경
 * 갱신이 이번에 그린 내용을 실제로 본다. */
static i64 hmje_gpu_end_present(void) {
    if (g_gpu_command_buffer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    /* 층이 열린 채로 present가 오면 기록이 짝이 안 맞은 것이다. 남겨 두면
     * 다음 프레임이 엉뚱한 텍스처에 그리므로 깊이를 여기서 되돌린다. */
    g_gpu_layer_depth = 0;
    hmje_gpu_end_render_encoder();
    /* GPU 완료를 기다리지 않는다. 한 큐라 배경→화면→다음 프레임 순서가 GPU에서
     * 보장되고, 명령 버퍼는 쓰는 텍스처(글리프·층·이미지 업로드)를 끝날 때까지
     * 붙든다. 기다리면 배경 트랜잭션의 GPU 실행 동안 CPU가 화면 트랜잭션을 기록하지
     * 못하고, 화면 트랜잭션 뒤에는 다음 입력 처리를 못 한다. 실행 실패는 완료
     * 처리기가 대상 비트로 남긴다(BEGIN·COMPOSITE_TARGET 참고). 앞선 drawable이
     * 모자라면 nextDrawable이 기다려 CPU가 GPU를 무한히 앞서지 않는다. */
    HmjeHostWindow *window = HMJE_WIN;
    id<MTLCommandBuffer> commands = g_gpu_command_buffer;
    g_gpu_command_buffer = nil;
    if (g_gpu_current_target == 1) {
        [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if (completed.status == MTLCommandBufferStatusError)
                atomic_fetch_or_explicit(&window->w_gpu_async_failed, 2u, memory_order_release);
        }];
        [commands commit];
        /* 덤프는 같은 큐의 읽기 명령을 기다리므로 이 트랜잭션 뒤의 내용을 본다. */
        dump_gpu_target_bg_debug();
        return 1;
    }
    u64 drawable_wait_start = latency_enabled() ? latency_now_us() : 0;
    id<CAMetalDrawable> drawable=[g_layer nextDrawable];
    if (drawable_wait_start != 0) {
        u64 waited = latency_now_us() - drawable_wait_start;
        if (waited >= 1000) fprintf(stderr, "posix-host-drawable-wait: us=%llu\n", (unsigned long long)waited);
    }
    if (drawable == nil) { [commands commit]; return 1; }
    u64 width=(u64)MIN(g_gpu_target.width, drawable.texture.width);
    u64 height=(u64)MIN(g_gpu_target.height, drawable.texture.height);
    id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
    [blit copyFromTexture:g_gpu_target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
               sourceSize:MTLSizeMake((NSUInteger)width,(NSUInteger)height,1)
                toTexture:drawable.texture destinationSlice:0 destinationLevel:0
       destinationOrigin:MTLOriginMake(0,0,0)];
    [blit endEncoding];
    [commands presentDrawable:drawable];
    /* 지연 계측이 켜져 있으면 이 drawable이 실제로 화면에 나온 시각(presentedTime, 주사
     * 경계)도 남긴다 — GPU 완료 시각과 달리 표시 페이싱·빠진 주사를 그대로 보여 준다.
     * 0이면 합성기가 그 프레임을 버렸다(다음 프레임에 밀림). */
    if (latency_enabled()) {
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            CFTimeInterval at = shown.presentedTime;
            fprintf(stderr, "posix-host-shown: t=%llu\n", at > 0.0 ? (unsigned long long)(at * 1000000.0) : 0ull);
        }];
    }
    /* 지연 계측은 화면 트랜잭션이 GPU에서 끝난 시각으로 잰다(기다리지 않아도 같은 정의). */
    latency_capture latency = latency_take_pending();
    BOOL reportDebug = getenv("HMJE_POSIX_DEBUG_GPU") != NULL;
    u64 reportWidth = g_width, reportHeight = g_height, reportScale = g_scale;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError) {
            atomic_fetch_or_explicit(&window->w_gpu_async_failed, 1u, memory_order_release);
            return;
        }
        latency_report(latency);
        static atomic_bool reported = false;
        if (reportDebug && !atomic_exchange(&reported, true)) {
            fprintf(stderr, "posix-host-gpu: direct command rendering active (%llux%llu, scale=%llu)\n",
                    reportWidth, reportHeight, reportScale);
        }
    }];
    [commands commit];
    show_window(g_win);
    hmje_vsync_presented(g_win);
    pump_events();
    /* HMJE_POSIX_DUMP_FRAME은 원래 CPU framebuffer 경로(dump_frame_debug)만
     * 덮었다 — GPU 직접 렌더 프레임은 g_backbuffer를 안 건드리므로 그쪽
     * 덤프가 비어있었다. 검증 편의를 위해 같은 이스케이프 해치로 GPU
     * persistent target(g_gpu_target)도 덤프한다(env var 미설정이면 무비용). */
    dump_gpu_target_debug();
    return 1;
}

/* HMJE_GPU_LAYER_BEGIN: [x, y, 너비, 높이, 불투명도] 40바이트.
 *
 * 그룹 불투명도의 GPU 절반. 서브트리를 층 텍스처에 그린
 * 뒤 한 번에 얹어야 겹친 자식이 두 번 섞이지 않는다.
 *
 * 층을 투명하게 시작하지 않고 **현재 대상의 해당 영역을 그대로 떠서** 시작하는
 * 것이 핵심이다 — 그러면 알파 채널을 따로 관리하지 않아도 결과가 정확하고,
 * CPU 재생기(hmje-ui command_interpreter.hmje)와 정확히 같은 모델이 된다.
 *
 * 층 텍스처의 UV는 항상 0..1 전체 범위를 읽으므로 슬롯의 이전 텍스처가 더
 * 크면 현재 층으로 축소 샘플링되어 과거 픽셀이 섞인다. 따라서 크기가 정확히
 * 같을 때만 재사용한다. 한 프레임에 반투명 뷰가 수십 개 있어도 텍스처는
 * 중첩 깊이만큼만 존재한다. */
static i64 hmje_gpu_layer_begin(const u64 *r, u64 length) {
    if (length != 40 || r == NULL || g_gpu_command_buffer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_gpu_layer_depth >= HMJE_GPU_LAYER_MAX) return HMJE_POSIX_ENOSYS_LOCAL;
    id<MTLTexture> source = hmje_gpu_active_target();
    if (source == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 x = r[0], y = r[1], width = r[2], height = r[3], alpha = r[4];
    if (x >= (u64)source.width || y >= (u64)source.height) return HMJE_POSIX_ENOSYS_LOCAL;
    if (width > (u64)source.width - x) width = (u64)source.width - x;
    if (height > (u64)source.height - y) height = (u64)source.height - y;
    if (width == 0 || height == 0) return HMJE_POSIX_ENOSYS_LOCAL;

    int slot = g_gpu_layer_depth;
    id<MTLTexture> layer = g_gpu_layer_texture[slot];
    if (layer == nil || (u64)layer.width != width || (u64)layer.height != height) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
        description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        layer = [g_device newTextureWithDescriptor:description];
        if (layer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
        g_gpu_layer_texture[slot] = layer;
    }

    hmje_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_gpu_command_buffer blitCommandEncoder];
    [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                  sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                   toTexture:layer destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];

    g_gpu_layer_saved_target[slot] = source;
    g_gpu_layer_saved_clip[slot] = g_gpu_clip;
    g_gpu_layer_x[slot] = x;
    g_gpu_layer_y[slot] = y;
    g_gpu_layer_w[slot] = width;
    g_gpu_layer_h[slot] = height;
    g_gpu_layer_alpha[slot] = alpha > 255 ? 255 : alpha;
    g_gpu_layer_depth = slot + 1;
    /* 층 좌표계로 옮긴 클립. 층 밖은 애초에 존재하지 않으므로 전체로 연다. */
    g_gpu_clip = (MTLScissorRect){0, 0, (NSUInteger)width, (NSUInteger)height};
    return 1;
}

/* HMJE_GPU_LAYER_END: 페이로드 없음. 층을 불투명도로 얹고 대상을 되돌린다. */
static i64 hmje_gpu_layer_end(u64 length) {
    if (length != 0 || g_gpu_command_buffer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_gpu_layer_depth <= 0) return 1;
    int slot = --g_gpu_layer_depth;
    id<MTLTexture> layer = g_gpu_layer_texture[slot];
    id<MTLTexture> target = g_gpu_layer_saved_target[slot];
    g_gpu_clip = g_gpu_layer_saved_clip[slot];
    g_gpu_layer_saved_target[slot] = nil;
    if (layer == nil || target == nil) return HMJE_POSIX_ENOSYS_LOCAL;

    hmje_gpu_end_render_encoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [g_gpu_command_buffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder setScissorRect:g_gpu_clip];
    HmjeGpuUniforms uniforms;
    memset(&uniforms, 0, sizeof(uniforms));
    uniforms.origin[0] = (float)g_gpu_layer_x[slot];
    uniforms.origin[1] = (float)g_gpu_layer_y[slot];
    uniforms.size[0] = (float)g_gpu_layer_w[slot];
    uniforms.size[1] = (float)g_gpu_layer_h[slot];
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    uniforms.color[3] = (float)g_gpu_layer_alpha[slot] / 255.0f;
    [encoder setRenderPipelineState:g_gpu_layer_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:layer atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    *hmje_gpu_active_needs_clear_flag() = NO;
    return 1;
}

static i64 hmje_gpu_abort(void) {
    g_gpu_layer_depth = 0;
    if (g_gpu_command_buffer == nil) return 1;
    hmje_gpu_end_render_encoder();
    g_gpu_command_buffer = nil;
    return 1;
}

i64 hmje_posix_gpu_command(i64 kind, i64 request_address, i64 request_length) {
    @autoreleasepool {
        const u64 *request=(const u64 *)(uintptr_t)(u64)request_address;
        u64 length=request_length < 0 ? 0 : (u64)request_length;
        if (kind == 0) {
            if (length != 32 || request == NULL) return HMJE_POSIX_ENOSYS_LOCAL;
            u64 *output=(u64 *)(uintptr_t)(u64)request_address;
            output[0]=2; output[1]=1; output[2]=UINT64_C(0x000000000000fffe); output[3]=0;
            return 32;
        }
        i64 result = HMJE_POSIX_ENOSYS_LOCAL;
        switch (kind) {
        case HMJE_GPU_BEGIN: result=hmje_gpu_begin(request,length); break;
        case HMJE_GPU_CLIP: result=hmje_gpu_set_clip(request,length); break;
        case HMJE_GPU_UPLOAD_GLYPH: result=hmje_gpu_upload_glyph(request,length); break;
        case HMJE_GPU_DRAW: result=hmje_gpu_draw(request,length); break;
        case HMJE_GPU_IMAGE_BLIT: result=hmje_gpu_image_blit(request,length); break;
        case HMJE_GPU_IMAGE_SCALE_BLIT: result=hmje_gpu_image_scale_blit(request,length); break;
        case HMJE_GPU_LAYER_BEGIN: result=hmje_gpu_layer_begin(request,length); break;
        case HMJE_GPU_LAYER_END: result=hmje_gpu_layer_end(length); break;
        case HMJE_GPU_END_PRESENT: result=hmje_gpu_end_present(); break;
        case HMJE_GPU_ABORT: result=hmje_gpu_abort(); break;
        case HMJE_GPU_SCROLL_SHIFT: result=hmje_gpu_scroll_shift(request,length); break;
        case HMJE_GPU_PLANAR_SHIFT: result=hmje_gpu_planar_shift(request,length); break;
        case HMJE_GPU_COMPOSITE_TARGET: result=hmje_gpu_composite_target(request,length); break;
        case HMJE_GPU_BACKDROP_BLUR: result=hmje_gpu_backdrop_blur(request,length); break;
        case HMJE_GPU_GRADIENT_FILL: result=hmje_gpu_gradient_fill(request,length); break;
        default: break;
        }
        return result;
    }
}

/* ---- posix_host_shim.c 가 부르는 기존 framebuffer/input 함수 ------------ */

i64 hmje_posix_framebuffer_info(i64 out_address, i64 out_length) {
    ensure_window();
    if (out_length < 48) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    u64 *out = (u64 *)(uintptr_t)(u64)out_address;
    out[0] = g_width;
    out[1] = g_height;
    out[2] = g_pitch;
    out[3] = 4;  /* pixel_bytes */
    out[4] = 1;  /* FRAMEBUFFER_FORMAT_XRGB8888 (canonical) */
    out[5] = 1;  /* FRAMEBUFFER_FLAG_PRESENT */
    if (out_length >= 56) {
        out[6] = g_scale; /* v2 확장: FRAMEBUFFER_INFO_SCALE_OFFSET(48) */
        return 56;
    }
    return 48;
}

i64 hmje_posix_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y) {
    ensure_window();
    if (width <= 0 || height <= 0 || pitch <= 0) {
        return 0;
    }
    u64 uw = (u64)width, uh = (u64)height, upitch = (u64)pitch;
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

/* HMJE_POSIX_DUMP_FRAME은 한 파일을 매 프레임 덮어쓴다 — 마지막 프레임만 남는다.
 * 상호작용 중 **스쳐 가는** 프레임(호버 damage가 한두 프레임만 깨지는 것 같은)은
 * 그걸로 못 잡는다. HMJE_POSIX_DUMP_FRAME_DIR을 주면 프레임마다 번호를 붙여
 * <dir>/frame-000042.png로 남긴다 — 두 실행을 프레임 대 프레임으로 견주는
 * parity 검사의 재료다(semtleOS tools/ui_frame_parity_check.py). 디렉터리는
 * 부르는 쪽이 미리 만든다.
 *
 * 한 프레임에 덤프 함수가 둘 다 도는 일은 없다 — GPU 프레임은 present를
 * 건너뛰므로 GPU 덤프만, CPU 프레임은 CPU 덤프만 돈다. 그래서 카운터 하나면 된다.
 *
 * **셀프테스트 포인터가 돌 때는 번호를 present가 아니라 포인터 동작에 맞춘다.**
 * present 수로 세면 두 실행이 어긋난다 — 렌더가 느린 쪽이 같은 입력에 present를
 * 덜/더 하기 때문이다. 동작 번호로 세면 한 동작에
 * 여러 번 present해도 같은 파일을 덮어써서 **그 동작의 정착된 프레임**이 남고,
 * 두 실행의 같은 번호가 같은 포인터 위치를 뜻한다. */

static const char *dump_frame_path(char *buffer, size_t size) {
    /* 셀프테스트 중에는 앞 창의 프레임만 남긴다(사람이 보는 창). */
    if (g_selftest_pointer_active && g_win != selftest_target()) return NULL;
    const char *directory = getenv("HMJE_POSIX_DUMP_FRAME_DIR");
    if (directory != NULL) {
        u64 index = g_selftest_pointer_active ? g_selftest_step_index : g_dump_present_index++;
        snprintf(buffer, size, "%s/frame-%06llu.png", directory, (unsigned long long)index);
        return buffer;
    }
    return getenv("HMJE_POSIX_DUMP_FRAME");
}

static void dump_frame_debug(void) {
    if (g_backbuffer == NULL) {
        return;
    }
    char path_buffer[1024];
    const char *path = dump_frame_path(path_buffer, sizeof path_buffer);
    if (path == NULL) {
        return;
    }
    @autoreleasepool {
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, g_backbuffer, (size_t)(g_pitch * g_height), NULL);
        CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
        CGImageRef image = CGImageCreate((size_t)g_width, (size_t)g_height, 8, 32, (size_t)g_pitch,
                                          colorSpace, bitmapInfo, provider, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (CFStringRef)UTTypePNG.identifier, 1, NULL);
        if (dest != NULL) {
            CGImageDestinationAddImage(dest, image, NULL);
            CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CFRelease(url);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
    }
}

/* dump_frame_debug와 같은 HMJE_POSIX_DUMP_FRAME 이스케이프 해치를 GPU 직접 렌더
 * persistent target(g_gpu_target, MTLStorageModePrivate)에도 적용한다 — CPU에서
 * 직접 못 읽으니 shared MTLBuffer로 한 번 blit-readback한 뒤 같은 PNG 인코딩을 쓴다.
 * g_gpu_target은 BGRA8Unorm이라 dump_frame_debug와 동일한 바이트 배치(B,G,R,X)이므로
 * 같은 kCGImageAlphaNoneSkipFirst 해석이 그대로 맞는다. */
static void dump_gpu_target_debug(void) {
    if (g_gpu_target == nil) {
        return;
    }
    char path_buffer[1024];
    const char *path = dump_frame_path(path_buffer, sizeof path_buffer);
    if (path == NULL) {
        return;
    }
    @autoreleasepool {
        NSUInteger width = g_gpu_target.width, height = g_gpu_target.height;
        NSUInteger rowBytes = width * 4;
        id<MTLBuffer> readback = [g_device newBufferWithLength:rowBytes * height
                                                         options:MTLResourceStorageModeShared];
        if (readback == nil) return;
        id<MTLCommandBuffer> commandBuffer = [g_queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
        [encoder copyFromTexture:g_gpu_target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                         toBuffer:readback destinationOffset:0
              destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * height];
        [encoder endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, readback.contents, rowBytes * height, NULL);
        CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
        CGImageRef image = CGImageCreate(width, height, 8, 32, rowBytes,
                                          colorSpace, bitmapInfo, provider, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (CFStringRef)UTTypePNG.identifier, 1, NULL);
        if (dest != NULL) {
            CGImageDestinationAddImage(dest, image, NULL);
            CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CFRelease(url);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
    }
}

/* dump_gpu_target_debug의 배경(target 1) 버전 — 별도 env var로 구분한다.
 * 화면 target과 달리 배경은 매 프레임 present되지 않으므로(END_PRESENT가
 * drawable을 건드리지 않음) 기존 HMJE_POSIX_DUMP_FRAME 훅에 얹으면 "이번
 * 프레임에 배경이 실제로 GPU에 그렸는지" 구분이 안 된다 — 진단 앱이 두
 * target을 독립적으로 읽어 비교할 수 있도록 경로를 나눈다. */
static void dump_gpu_target_bg_debug(void) {
    const char *path = getenv("HMJE_POSIX_DUMP_GPU_BG_FRAME");
    if (path == NULL || g_gpu_target_bg == nil) {
        return;
    }
    @autoreleasepool {
        NSUInteger width = g_gpu_target_bg.width, height = g_gpu_target_bg.height;
        NSUInteger rowBytes = width * 4;
        id<MTLBuffer> readback = [g_device newBufferWithLength:rowBytes * height
                                                         options:MTLResourceStorageModeShared];
        if (readback == nil) return;
        id<MTLCommandBuffer> commandBuffer = [g_queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
        [encoder copyFromTexture:g_gpu_target_bg sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                         toBuffer:readback destinationOffset:0
              destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * height];
        [encoder endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, readback.contents, rowBytes * height, NULL);
        CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
        CGImageRef image = CGImageCreate(width, height, 8, 32, rowBytes,
                                          colorSpace, bitmapInfo, provider, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (CFStringRef)UTTypePNG.identifier, 1, NULL);
        if (dest != NULL) {
            CGImageDestinationAddImage(dest, image, NULL);
            CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CFRelease(url);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
    }
}

/* Metal present: 백버퍼(staging MTLBuffer)를 drawable 텍스처로 blit → vsync 표시.
 * nextDrawable이 displaySync 페이싱을 겸한다. 반환 직후 앱이 staging 버퍼를
 * 다시 쓰기 시작하므로 waitUntilCompleted로 GPU 복사 완료를 기다린다. */


static i64 present_metal(void) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (drawable == nil) {
            return 1; /* 일시적(창 최소화 등) — 프레임만 건너뛴다 */
        }
        id<MTLTexture> texture = [drawable texture];
        u64 copy_w = g_width, copy_h = g_height;
        if ((u64)[texture width] < copy_w) {
            copy_w = (u64)[texture width];
        }
        if ((u64)[texture height] < copy_h) {
            copy_h = (u64)[texture height];
        }
        if (copy_w == 0 || copy_h == 0) {
            return 1;
        }
        id<MTLCommandBuffer> command_buffer = [g_queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
        [encoder copyFromBuffer:g_staging
                   sourceOffset:0
              sourceBytesPerRow:(NSUInteger)g_pitch
            sourceBytesPerImage:0
                     sourceSize:MTLSizeMake((NSUInteger)copy_w, (NSUInteger)copy_h, 1)
                      toTexture:texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [encoder endEncoding];
        [command_buffer presentDrawable:drawable];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
    }
    return 1;
}

i64 hmje_posix_framebuffer_present(i64 rects_address, i64 count) {
    ensure_window();
    (void)rects_address;
    if (count <= 0) {
        return 0;
    }
    if (g_device != nil) {
        present_metal();
    } else {
        @autoreleasepool {
            [g_view setNeedsDisplay:YES];
            [g_view displayIfNeeded];
        }
    }
    latency_mark_present();
    show_window(g_win);
    hmje_vsync_presented(g_win);
    pump_events();
    dump_frame_debug();
    return 1;
}

i64 hmje_posix_input_poll(void) {
    ensure_window();
    g_runtime_polling = YES;
    pump_events();
    if (g_close_requested && !g_close_delivered) {
        g_close_delivered = YES;
        return (i64)(HMJE_INPUT_KIND_APP_CLOSE << HMJE_INPUT_KIND_SHIFT);
    }
    /* 기하 변경이 큐의 어떤 이벤트보다 먼저다 — 뒤 이벤트들의 좌표가 새
     * 표면 기준이기도 하고, 리사이즈 통지가 밀리면 그 프레임 전체가 옛
     * 크기로 그려진다. */
    i64 resize = surface_resize_event_if_needed();
    if (resize != 0) {
        return resize;
    }
    i64 event = key_ring_pop();
    if (event == 0 && g_win == selftest_target()) {
        /* 셀프테스트는 앞(키) 창의 빈 poll로만 프레임을 센다 — 창이 여럿이면
         * 프레임마다 poll도 여럿이다. */
        maybe_selftest_resize();
        maybe_selftest_pointer();
        hmje_posix_text_input_selftest_tick();
        /* 합성 포인터가 이번 프레임에 이벤트를 넣었을 수 있다 — 앱이 다음
         * 프레임까지 기다리지 않게 바로 하나 꺼내 준다. */
        event = key_ring_pop();
    }
    return event;
}

i64 hmje_posix_input_poll2(i64 out_address, i64 out_length) {
    ensure_window();
    pump_events();
    return hmje_pointer_ring_pop(&g_pointer_samples, out_address, out_length);
}

/* Query the window environment without changing an explicit app/view override. */
i64 hmje_posix_system_theme(void) {
    @autoreleasepool {
        NSAppearance *appearance = g_window != nil ? g_window.effectiveAppearance : NSApp.effectiveAppearance;
        NSString *match = [appearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [match isEqualToString:NSAppearanceNameDarkAqua] ? 2 : 1;
    }
}
