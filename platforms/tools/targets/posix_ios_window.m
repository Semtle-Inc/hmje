/*
 * posix_ios_window.m — SYS_FRAMEBUFFER_* / SYS_INPUT_POLL을 실제 UIKit 창으로 구현한다.
 * posix_host_shim.c의 syscall 디스패치에서 호출하는 framebuffer/input 함수와
 * 자원 backing 오버라이드, 실제 프로세스 main()을 제공한다.
 *
 * UIApplicationMain은 메인 스레드에서 호출되고 UIKit의 run loop를 소유한다.
 * HMJE 진입점은 별도 pthread에서 실행한다. SYS_FRAMEBUFFER_* / SYS_INPUT_POLL은
 * dispatch_sync(dispatch_get_main_queue(), ...)로 메인 스레드에 전달한다.
 * UIKit 객체는 메인 스레드에서만 접근한다.
 */



























#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <os/lock.h>

#include "pointer_input.h"
#include "legacy_input.h"
#include "legacy_primary_input.h"

typedef long long i64;
typedef unsigned long long u64;

#define HMJE_POSIX_ENOSYS_LOCAL (-38LL)

static UIWindow *g_window = nil;
static UIView *g_view = nil;
static UIViewController *g_root_controller = nil;
static u64 g_safe_top = 0;
static u64 g_safe_right = 0;
static u64 g_safe_bottom = 0;
static u64 g_safe_left = 0;

static u64 g_width = 0;
static u64 g_height = 0;
static u64 g_pitch = 0;
static u64 g_scale = 1;
static CGFloat g_keyboard_overlap = 0.0;
static BOOL g_keyboard_safe_area_ignored = NO;
static id<MTLDevice> g_device = nil;
static id<MTLCommandQueue> g_queue = nil;
static CAMetalLayer *g_metal_layer = nil;
/* canonical 0x00RRGGBB, LE u32 per pixel. CPU가 계속 갱신하는 완전한 현재
 * 프레임이다. Metal 업로드 메모리와 분리해야 present가 반환된 직후 다음
 * dirty blit이 들어와도 이미 커밋된 GPU 읽기와 경합하지 않는다. */
static unsigned char *g_backbuffer = NULL;

static dispatch_semaphore_t g_launch_sem;
/* vsync pacing: one CADisplayLink on the UIKit main run loop tells the HMJE
 * frame loop (a separate thread sleeping in SYS_SLEEP_MS) when a refresh
 * happened, so frames start on the refresh instead of on a free-running timer
 * (which beat against 60Hz and dropped a frame every 5-6). */
static CADisplayLink *g_display_link = nil;
extern void hmje_async_notify_frame(void);

#define HMJE_IOS_METAL_MAX_FRAMES_IN_FLIGHT 3
#define HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT HMJE_IOS_METAL_MAX_FRAMES_IN_FLIGHT
#define HMJE_IOS_METAL_PERMIT_WAIT_NS (50LL * NSEC_PER_MSEC)

/* Direct display-list transactions and software fallback presents share one
 * drawable/queue budget.  Separate 3-frame semaphores would allow six queued
 * presents and make CAMetalLayer.nextDrawable block the UIKit main queue.
 *
 * A completion handler is normally guaranteed to return its permit.  A wedged
 * GPU/driver, however, can leave every handler outstanding indefinitely.  Do
 * not turn that device failure into an HMJE app-thread deadlock: wait for one
 * bounded 50 ms interval, then make later callers fail fast until any old
 * command completes.  The caller keeps the canonical CPU canvas current and
 * retries on a later frame; the last drawable remains visible meanwhile.
 *
 * We deliberately do not manufacture a replacement permit.  Reusing its
 * staging/layer slot while the old command might still execute would create a
 * GPU data race.  A real completion signals the same semaphore and reopens the
 * path safely. */
static dispatch_semaphore_t g_ios_metal_in_flight = nil;
static atomic_bool g_ios_metal_permit_fail_fast = ATOMIC_VAR_INIT(false);
static atomic_bool g_ios_metal_permit_timeout_reported = ATOMIC_VAR_INIT(false);

static BOOL hmje_ios_metal_try_take_in_flight_permit(void) {
    if (g_ios_metal_in_flight == nil) return NO;
    if (dispatch_semaphore_wait(g_ios_metal_in_flight, DISPATCH_TIME_NOW) != 0) {
        return NO;
    }
    atomic_store_explicit(&g_ios_metal_permit_fail_fast, false, memory_order_release);
    atomic_store_explicit(&g_ios_metal_permit_timeout_reported, false,
                          memory_order_release);
    return YES;
}

static BOOL hmje_ios_metal_take_in_flight_permit(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_ios_metal_in_flight =
            dispatch_semaphore_create(HMJE_IOS_METAL_MAX_FRAMES_IN_FLIGHT);
    });
    if (g_ios_metal_in_flight == nil) return NO;
    if (atomic_load_explicit(&g_ios_metal_permit_fail_fast, memory_order_acquire)) {
        return hmje_ios_metal_try_take_in_flight_permit();
    }
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW,
                                              HMJE_IOS_METAL_PERMIT_WAIT_NS);
    if (dispatch_semaphore_wait(g_ios_metal_in_flight, deadline) == 0) return YES;

    atomic_store_explicit(&g_ios_metal_permit_fail_fast, true, memory_order_release);
    /* Close the timeout-vs-completion race: a handler may have signalled just
     * after dispatch_semaphore_wait reported its timeout. */
    if (hmje_ios_metal_try_take_in_flight_permit()) return YES;
    if (!atomic_exchange_explicit(&g_ios_metal_permit_timeout_reported, true,
                                  memory_order_acq_rel)) {
        fprintf(stderr,
                "posix-ios-metal: GPU completion stalled; presentation will retry\n");
    }
    return NO;
}

static void hmje_ios_metal_release_in_flight_permit(void) {
    if (g_ios_metal_in_flight == nil) return;
    dispatch_semaphore_signal(g_ios_metal_in_flight);
    atomic_store_explicit(&g_ios_metal_permit_fail_fast, false, memory_order_release);
    atomic_store_explicit(&g_ios_metal_permit_timeout_reported, false,
                          memory_order_release);
}

/* CPU 폴백 present의 업로드 풀. 풀 객체 하나가 한 surface 크기 세대를
 * 나타낸다. 크기가 바뀌면 새 풀로 원자적으로 갈아 끼우고, 이전 command
 * buffer의 completion block이 옛 풀과 MTLBuffer/private texture를 붙잡으므로
 * resize 중에도 resource lifetime이 안전하다. 느린 shared-buffer upload는
 * drawable이 아니라 이 private texture에서 끝내고, 완성된 texture만 drawable로
 * 복사해 Simulator/Core Animation capture에도 반쪽 frame을 노출하지 않는다. */
@interface HmjeSoftwarePresentPool : NSObject {
@private
    NSUInteger _width;
    NSUInteger _height;
    NSUInteger _bytes_per_row;
    NSUInteger _length;
    __strong id<MTLBuffer> _buffers[HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT];
    __strong id<MTLTexture> _software_texture;
    os_unfair_lock _lock;
    unsigned _free_mask;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device
                         width:(NSUInteger)width
                        height:(NSUInteger)height
                   bytesPerRow:(NSUInteger)bytesPerRow;
- (BOOL)matchesWidth:(NSUInteger)width
              height:(NSUInteger)height
         bytesPerRow:(NSUInteger)bytesPerRow;
- (int)takeSlot;
- (void)releaseSlot:(int)slot;
- (id<MTLBuffer>)bufferAtSlot:(int)slot;
- (id<MTLTexture>)softwareTexture;
@end

@implementation HmjeSoftwarePresentPool

- (instancetype)initWithDevice:(id<MTLDevice>)device
                         width:(NSUInteger)width
                        height:(NSUInteger)height
                   bytesPerRow:(NSUInteger)bytesPerRow {
    self = [super init];
    if (self == nil || device == nil || width == 0 || height == 0 ||
        width > NSUIntegerMax / 4 ||
        bytesPerRow < width * 4 || height > NSUIntegerMax / bytesPerRow) {
        return nil;
    }
    _width = width;
    _height = height;
    _bytes_per_row = bytesPerRow;
    _length = height * bytesPerRow;
    _lock = OS_UNFAIR_LOCK_INIT;
    _free_mask = (1u << HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT) - 1u;
    for (int slot = 0; slot < HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT; slot++) {
        _buffers[slot] = [device newBufferWithLength:_length
                                             options:MTLResourceStorageModeShared];
        if (_buffers[slot] == nil) return nil;
    }
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width
                                    height:height
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    /* Every software command uses g_queue.  Its ordered command buffers plus
     * Metal's default tracked hazards make one generation texture safe across
     * the three outstanding immutable-buffer slots. */
    _software_texture = [device newTextureWithDescriptor:descriptor];
    if (_software_texture == nil) return nil;
    return self;
}

- (BOOL)matchesWidth:(NSUInteger)width
              height:(NSUInteger)height
         bytesPerRow:(NSUInteger)bytesPerRow {
    return _width == width && _height == height && _bytes_per_row == bytesPerRow;
}

- (int)takeSlot {
    if (!hmje_ios_metal_take_in_flight_permit()) return -1;
    os_unfair_lock_lock(&_lock);
    unsigned available = _free_mask;
    if (available == 0u) {
        os_unfair_lock_unlock(&_lock);
        hmje_ios_metal_release_in_flight_permit();
        return -1;
    }
    int slot = __builtin_ctz(available);
    _free_mask = available & ~(1u << slot);
    os_unfair_lock_unlock(&_lock);
    return slot;
}

- (void)releaseSlot:(int)slot {
    if (slot < 0 || slot >= HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT) return;
    unsigned bit = 1u << slot;
    BOOL released = NO;
    os_unfair_lock_lock(&_lock);
    if ((_free_mask & bit) == 0u) {
        _free_mask |= bit;
        released = YES;
    }
    os_unfair_lock_unlock(&_lock);
    if (released) hmje_ios_metal_release_in_flight_permit();
}

- (id<MTLBuffer>)bufferAtSlot:(int)slot {
    if (slot < 0 || slot >= HMJE_IOS_SOFTWARE_PRESENT_SLOT_COUNT) return nil;
    return _buffers[slot];
}

- (id<MTLTexture>)softwareTexture {
    return _software_texture;
}

@end

static HmjeSoftwarePresentPool *g_software_present_pool = nil;
/* When every Metal permit is held by outstanding command buffers, a CPU
 * fallback frame is accepted into the canonical g_backbuffer and retried by
 * the app loop's existing zero-rect idle flush.  State packs a 63-bit logical
 * presentation epoch and one pending bit.  A later direct GPU screen commit
 * advances the same epoch, so an older CPU completion can neither resurrect a
 * stale retry nor roll the drawable back after CPU-shadow replay was skipped. */
#define HMJE_IOS_SOFTWARE_PENDING_BIT UINT64_C(1)
#define HMJE_IOS_PRESENT_EPOCH_MAX (UINT64_MAX >> 1)
static atomic_ullong g_ios_present_state = ATOMIC_VAR_INIT(UINT64_C(2));

static u64 hmje_ios_present_next_epoch(void) {
    unsigned long long current = atomic_load_explicit(&g_ios_present_state,
                                                       memory_order_acquire);
    for (;;) {
        u64 epoch = ((u64)current >> 1) + 1;
        if (epoch == 0 || epoch > HMJE_IOS_PRESENT_EPOCH_MAX) epoch = 1;
        unsigned long long desired = (unsigned long long)(epoch << 1);
        if (atomic_compare_exchange_weak_explicit(&g_ios_present_state,
                                                  &current, desired,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return epoch;
        }
    }
}

static BOOL hmje_ios_present_pending_epoch(u64 *epoch_out) {
    u64 state = (u64)atomic_load_explicit(&g_ios_present_state,
                                          memory_order_acquire);
    if ((state & HMJE_IOS_SOFTWARE_PENDING_BIT) == 0) return NO;
    if (epoch_out != NULL) *epoch_out = state >> 1;
    return YES;
}

static void hmje_ios_present_set_pending_for_epoch(u64 epoch, BOOL pending) {
    unsigned long long current = atomic_load_explicit(&g_ios_present_state,
                                                       memory_order_acquire);
    for (;;) {
        if (((u64)current >> 1) != epoch) return;
        unsigned long long desired = (unsigned long long)(epoch << 1);
        if (pending) desired |= HMJE_IOS_SOFTWARE_PENDING_BIT;
        if (atomic_compare_exchange_weak_explicit(&g_ios_present_state,
                                                  &current, desired,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
}

static void hmje_ios_present_supersede_software(void) {
    (void)hmje_ios_present_next_epoch();
}

#define HMJE_INPUT_KIND_SHIFT HMJE_LEGACY_INPUT_KIND_SHIFT
#define HMJE_INPUT_KIND_POINTER_MOVE HMJE_LEGACY_INPUT_KIND_POINTER_MOVE
#define HMJE_INPUT_KIND_POINTER_BUTTON HMJE_LEGACY_INPUT_KIND_POINTER_BUTTON
#define HMJE_INPUT_KIND_KEY 0ULL
#define HMJE_INPUT_KIND_TEXT 4ULL
#define HMJE_INPUT_KIND_EDIT_COMMAND 7ULL
#define HMJE_INPUT_PRESSED_FLAG 256ULL
#define HMJE_INPUT_TEXT_CODEPOINT_MASK 2097151ULL
#define HMJE_INPUT_KIND_COMPOSITION 8ULL
#define HMJE_INPUT_COMPOSITION_END_FLAG 4194304ULL
#define HMJE_INPUT_COMPOSITION_CARET_MASK 65535ULL
#define HMJE_IOS_LEGACY_RING_CAP 256u
/* macOS posix_host_window.m과 같은 SURFACE_RESIZE 인코딩(비트 0-23 너비,
 * 24-47 높이, 48-55 배율) — 커널 syscall.hmje 계약과 값이 같아야 한다. */
#define HMJE_INPUT_KIND_SURFACE_RESIZE 5ULL
#define HMJE_INPUT_SURFACE_SIZE_MASK 16777215ULL
#define HMJE_INPUT_SURFACE_HEIGHT_SHIFT 24
#define HMJE_INPUT_SURFACE_SCALE_SHIFT 48
#define HMJE_INPUT_SURFACE_SCALE_MASK 255ULL
#define HMJE_INPUT_POINTER_COORD_MASK 16777215ull
#define HMJE_INPUT_POINTER_Y_SHIFT 24
#define HMJE_INPUT_POINTER_PRESSED_FLAG (1ull << 55)
#define HMJE_EDIT_SELECT_ALL 1ULL
#define HMJE_EDIT_COPY 2ULL
#define HMJE_EDIT_CUT 3ULL
#define HMJE_EDIT_PASTE 4ULL
#define HMJE_EDIT_UNDO 5ULL
#define HMJE_EDIT_REDO 6ULL

static hmje_pointer_ring g_pointer_samples;
static hmje_legacy_input_ring g_legacy_input;
static os_unfair_lock g_legacy_lock = OS_UNFAIR_LOCK_INIT;
static hmje_legacy_primary_state g_legacy_primary;

/* UIKit은 보통 한 접촉 동안 같은 UITouch 객체를 돌려주지만, Simulator의
 * 마우스 드래그/취소 경계에서는 마지막 콜백의 wrapper 주소가 달라질 수 있다.
 * 주소가 달라졌다는 이유로 UP을 버리면 legacy primary가 영구히 남고,
 * 그 뒤 모든 탭이 secondary touch로 오인돼 사라진다. 현재 UIEvent 안에 기존
 * primary가 실제로 살아 있는지를 따로 확인해, 사라진 경우만 안전하게 회수한다.
 */
static BOOL legacy_touch_identity_is_active(UIEvent *event, uintptr_t identity) {
    if (identity == 0 || event == nil) return NO;
    for (UITouch *active in event.allTouches) {
        if ((uintptr_t)active != identity) continue;
        if (active.phase != UITouchPhaseEnded && active.phase != UITouchPhaseCancelled) {
            return YES;
        }
    }
    return NO;
}

/* ---- 앱을 연 주소(SYS_APP_OPEN_URL 120) --------------------------------------
 * UIKit(주 스레드)이 쌓고 HMJE 스레드가 꺼낸다. 차가운 시작의 주소는 장면 연결 옵션
 * (URLContexts)으로, 도는 중의 주소는 scene:openURLContexts:로 온다. 앱 루프가 한 번씩
 * 꺼내 `URL이 열리면`을 부르므로 앱 코드가 처리기를 등록하기 전에 온 주소도 잃지 않는다. */
static os_unfair_lock g_open_url_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableArray<NSData *> *g_open_urls = nil;
#define HMJE_OPEN_URL_MAX_BYTES (1u << 20)
#define HMJE_OPEN_URL_MAX_PENDING 256

static void app_open_url_enqueue(NSURL *url) {
    if (url == nil || [url isFileURL]) return;
    NSData *bytes = [[url absoluteString] dataUsingEncoding:NSUTF8StringEncoding];
    if (bytes == nil || [bytes length] == 0 || [bytes length] > HMJE_OPEN_URL_MAX_BYTES) return;
    os_unfair_lock_lock(&g_open_url_lock);
    if (g_open_urls == nil) g_open_urls = [NSMutableArray array];
    if ([g_open_urls count] >= HMJE_OPEN_URL_MAX_PENDING) [g_open_urls removeObjectAtIndex:0];
    [g_open_urls addObject:bytes];
    os_unfair_lock_unlock(&g_open_url_lock);
    if (getenv("HMJE_POSIX_DEBUG_OPEN_URL") != NULL) fprintf(stderr, "posix-ios-window: open-url queued\n");
}

static void app_open_url_contexts(NSSet<UIOpenURLContext *> *contexts) {
    for (UIOpenURLContext *context in contexts) app_open_url_enqueue(context.URL);
}

/* a1=0: 다음 주소의 바이트 길이(없으면 0). a1=1: 다음 주소를 a2(용량 a3)에 복사하고
 * 큐에서 뺀다. 용량이 모자라면 복사하지 않고 -34(ERANGE)다. */
i64 hmje_posix_app_open_url(i64 operation, i64 address, i64 capacity) {
    if (operation != 0 && operation != 1) return -22;
    i64 result = 0;
    os_unfair_lock_lock(&g_open_url_lock);
    NSData *head = [g_open_urls count] > 0 ? [g_open_urls objectAtIndex:0] : nil;
    if (head == nil) {
        result = 0;
    } else if (operation == 0) {
        result = (i64)[head length];
    } else if (address == 0 || capacity < (i64)[head length]) {
        result = -34;
    } else {
        memcpy((void *)(uintptr_t)(u64)address, [head bytes], [head length]);
        result = (i64)[head length];
        [g_open_urls removeObjectAtIndex:0];
    }
    os_unfair_lock_unlock(&g_open_url_lock);
    return result;
}

static void legacy_input_push(u64 value) {
    os_unfair_lock_lock(&g_legacy_lock);
    hmje_legacy_input_ring_push(&g_legacy_input, value);
    os_unfair_lock_unlock(&g_legacy_lock);
}

static void legacy_edit_command_push(u64 command) {
    legacy_input_push((HMJE_INPUT_KIND_EDIT_COMMAND << HMJE_INPUT_KIND_SHIFT)
                      | (command & 255ULL));
}

static void legacy_text_push_string(NSString *string) {
    if (string == nil || string.length == 0) return;
    NSData *utf32 = [string dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (utf32 == nil) return;
    const unsigned char *bytes = (const unsigned char *)utf32.bytes;
    NSUInteger count = utf32.length / 4;
    for (NSUInteger index = 0; index < count; index++) {
        u64 codepoint = (u64)bytes[index * 4]
                      | ((u64)bytes[index * 4 + 1] << 8)
                      | ((u64)bytes[index * 4 + 2] << 16)
                      | ((u64)bytes[index * 4 + 3] << 24);
        if (codepoint == 0 || codepoint > HMJE_INPUT_TEXT_CODEPOINT_MASK) continue;
        legacy_input_push((HMJE_INPUT_KIND_TEXT << HMJE_INPUT_KIND_SHIFT)
                          | (codepoint & HMJE_INPUT_TEXT_CODEPOINT_MASK));
    }
}

/* KIND_COMPOSITION: the marked text UIKit is composing (code points, then
 * END with the caret in code points). An empty string ends the preedit. */
static void legacy_composition_push(NSString *string, NSUInteger caretUTF16) {
    NSUInteger length = string.length;
    NSUInteger index = 0;
    u64 caret = 0;
    while (index < length) {
        unichar unit = [string characterAtIndex:index];
        u64 codepoint = unit;
        NSUInteger width = 1;
        if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < length) {
            unichar low = [string characterAtIndex:index + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                codepoint = 0x10000 + (((u64)unit - 0xd800) << 10) + ((u64)low - 0xdc00);
                width = 2;
            }
        }
        if (index < caretUTF16) caret++;
        legacy_input_push((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT)
                          | (codepoint & HMJE_INPUT_TEXT_CODEPOINT_MASK));
        index += width;
    }
    if (caret > HMJE_INPUT_COMPOSITION_CARET_MASK) caret = HMJE_INPUT_COMPOSITION_CARET_MASK;
    legacy_input_push((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT)
                      | HMJE_INPUT_COMPOSITION_END_FLAG | caret);
}

static void legacy_key_push(u64 scanCode, BOOL pressed) {
    u64 event = (HMJE_INPUT_KIND_KEY << HMJE_INPUT_KIND_SHIFT) | (scanCode & 255ULL);
    if (pressed) event |= HMJE_INPUT_PRESSED_FLAG;
    legacy_input_push(event);
}

/* UIKit의 UIKeyInput만 직접 구현하면 한국어처럼 marked-text replacement가
 * 필요한 IME에 문서/선택 범위를 제공하지 못한다. 실제 UIKit 편집기를 보이지
 * 않는 입력 프록시로 두고, 그 편집기가 계산한 replacement delta만 HMJE의
 * 플랫폼 독립 편집 이벤트로 옮긴다. 화면과 캐럿은 계속 HMJE가 그린다. */
@interface HmjeTextInputProxy : UITextView <UITextViewDelegate>
@property(nonatomic, assign) BOOL suppressForwarding;
/* HMJE is showing a KIND_COMPOSITION preedit from this proxy. */
@property(nonatomic, assign) BOOL composing;
@property(nonatomic, copy) NSString *lastCommittedText;
@end

@implementation HmjeTextInputProxy

/* Hardware Shift+Return is a newline request, never an Enter action. */
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    for (UIPress *press in presses) {
        UIKey *key = press.key;
        if (key != nil && key.keyCode == UIKeyboardHIDUsageKeyboardReturnOrEnter &&
            (key.modifierFlags & UIKeyModifierShift) && !self.composing && self.markedTextRange == nil) {
            legacy_input_push(251ULL | 256ULL | (1ULL << 16));
            legacy_input_push(251ULL | (1ULL << 16));
            return;
        }
    }
    [super pressesBegan:presses withEvent:event];
}

- (instancetype)initWithFrame:(CGRect)frame textContainer:(NSTextContainer *)textContainer {
    self = [super initWithFrame:frame textContainer:textContainer];
    if (self != nil) {
        self.delegate = self;
        self.backgroundColor = [UIColor clearColor];
        self.textColor = [UIColor clearColor];
        self.tintColor = [UIColor clearColor];
        self.alpha = 0.01;
        self.scrollEnabled = NO;
        /* Hidden views cannot participate in the text-input system. Keep the
         * proxy interactive but move its one-pixel frame outside the canvas. */
        self.userInteractionEnabled = YES;
        self.accessibilityElementsHidden = YES;
        /* The native editor owns language services even though HMJE owns the
         * pixels.  Disabling these made the keyboard little more than a raw
         * key source: predictions, autocorrection and smart punctuation never
         * got a chance to replace the proxy document.  The document-diff
         * bridge below already represents replacements as delete+insert, so
         * use the user's system defaults just like a visible UITextView. */
        self.autocorrectionType = UITextAutocorrectionTypeDefault;
        self.spellCheckingType = UITextSpellCheckingTypeDefault;
        self.smartDashesType = UITextSmartDashesTypeDefault;
        self.smartQuotesType = UITextSmartQuotesTypeDefault;
        self.smartInsertDeleteType = UITextSmartInsertDeleteTypeDefault;
        self.textContentType = nil;
        self.keyboardType = UIKeyboardTypeDefault;
        self.returnKeyType = UIReturnKeyDefault;
    }
    return self;
}

/* The proxy owns native text for IME composition, never a clipboard source.
 * Password paste remains an input operation; copy/cut/share cannot export it. */
- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (self.isSecureTextEntry && (action == @selector(copy:) || action == @selector(cut:))) return NO;
    if (action == @selector(selectAll:) || action == @selector(copy:) ||
        action == @selector(cut:) || action == @selector(paste:) ||
        action == @selector(undo:) || action == @selector(redo:)) return YES;
    return [super canPerformAction:action withSender:sender];
}
- (void)selectAll:(id)sender { (void)sender; legacy_edit_command_push(HMJE_EDIT_SELECT_ALL); }
- (void)copy:(id)sender {
    (void)sender;
    if (!self.isSecureTextEntry) legacy_edit_command_push(HMJE_EDIT_COPY);
}
- (void)cut:(id)sender {
    (void)sender;
    if (!self.isSecureTextEntry) {
        legacy_edit_command_push(HMJE_EDIT_CUT);
        [self resetDocument];
    }
}
- (void)paste:(id)sender {
    (void)sender;
    legacy_edit_command_push(HMJE_EDIT_PASTE);
    [self resetDocument];
}
- (void)undo:(id)sender {
    (void)sender;
    legacy_edit_command_push(HMJE_EDIT_UNDO);
    [self resetDocument];
}
- (void)redo:(id)sender {
    (void)sender;
    legacy_edit_command_push(HMJE_EDIT_REDO);
    [self resetDocument];
}

- (void)resetDocument {
    [self endComposition];
    self.suppressForwarding = YES;
    self.text = @"";
    self.selectedRange = NSMakeRange(0, 0);
    self.lastCommittedText = @"";
    self.suppressForwarding = NO;
}

/* Return a UTF-16 prefix boundary that never cuts a composed character.
 * UIKit ranges use UTF-16 offsets, while HMJE edits in grapheme-sized delete
 * steps. Keeping the boundary on both contracts prevents surrogate pairs and
 * decomposed Hangul from being split by the bridge. */
- (NSUInteger)committedPrefixLengthFrom:(NSString *)before to:(NSString *)after {
    NSUInteger limit = MIN(before.length, after.length);
    NSUInteger prefix = 0;
    while (prefix < limit && [before characterAtIndex:prefix] == [after characterAtIndex:prefix]) {
        prefix += 1;
    }
    if (prefix > 0 && prefix < before.length) {
        NSRange sequence = [before rangeOfComposedCharacterSequenceAtIndex:prefix - 1];
        if (NSMaxRange(sequence) > prefix) prefix = sequence.location;
    }
    if (prefix > 0 && prefix < after.length) {
        NSRange sequence = [after rangeOfComposedCharacterSequenceAtIndex:prefix - 1];
        if (NSMaxRange(sequence) > prefix) prefix = sequence.location;
    }
    return prefix;
}

- (void)forwardCommittedDocument:(NSString *)document {
    NSString *before = self.lastCommittedText ?: @"";
    NSString *after = document ?: @"";
    if ([before isEqualToString:after]) return;

    NSUInteger prefix = [self committedPrefixLengthFrom:before to:after];
    NSString *removed = [before substringFromIndex:prefix];
    __block NSUInteger graphemeCount = 0;
    [removed enumerateSubstringsInRange:NSMakeRange(0, removed.length)
                                options:NSStringEnumerationByComposedCharacterSequences
                             usingBlock:^(__unused NSString *substring,
                                          __unused NSRange substringRange,
                                          __unused NSRange enclosingRange,
                                          __unused BOOL *stop) {
        graphemeCount += 1;
    }];
    for (NSUInteger index = 0; index < graphemeCount; index++) {
        legacy_key_push(51ULL, YES);
        legacy_key_push(51ULL, NO);
    }
    legacy_text_push_string([after substringFromIndex:prefix]);
    self.lastCommittedText = [after copy];
}

- (BOOL)textView:(UITextView *)textView
    shouldChangeTextInRange:(NSRange)range
           replacementText:(NSString *)replacement {
    if (self.suppressForwarding) return YES;
    if ([replacement isEqualToString:@"\n"] || [replacement isEqualToString:@"\r"]) {
        if (self.composing || textView.markedTextRange != nil) {
            [textView unmarkText];
            [self textViewDidChange:textView];
            legacy_input_push(251ULL | 256ULL | (1ULL << 21));
            legacy_key_push(251ULL, NO);
            return NO;
        }
        legacy_key_push(251ULL, YES);
        legacy_key_push(251ULL, NO);
        return NO;
    }
    /* Do not forward replacementText here. During Korean composition UIKit
     * repeatedly calls this delegate with the current marked-text delta. If
     * each delta is committed to HMJE, the system IME is effectively reset on
     * every keystroke and `안녕` arrives as `ㅇ ㅏ ㄴ ㄴ ㅕ ㅇ`.
     * textViewDidChange forwards only after markedTextRange disappears. */
    (void)textView;
    (void)range;
    return YES;
}

- (void)endComposition {
    if (!self.composing) return;
    self.composing = NO;
    legacy_composition_push(@"", 0);
}

- (void)textViewDidChange:(UITextView *)textView {
    if (self.suppressForwarding) return;
    UITextRange *marked = textView.markedTextRange;
    if (marked != nil) {
        /* The syllable being composed: HMJE draws it at its caret. */
        NSInteger start = [textView offsetFromPosition:textView.beginningOfDocument toPosition:marked.start];
        NSInteger end = [textView offsetFromPosition:textView.beginningOfDocument toPosition:marked.end];
        NSString *text = textView.text ?: @"";
        if (start >= 0 && end >= start && (NSUInteger)end <= text.length) {
            NSRange selected = textView.selectedRange;
            NSUInteger caret = (NSUInteger)(end - start);
            if (selected.location != NSNotFound && selected.location >= (NSUInteger)start
                && NSMaxRange(selected) <= (NSUInteger)end)
                caret = NSMaxRange(selected) - (NSUInteger)start;
            legacy_composition_push([text substringWithRange:NSMakeRange((NSUInteger)start, (NSUInteger)(end - start))], caret);
            self.composing = YES;
        }
        return;
    }
    /* The commit (or its absence, for a cancel) replaces the preedit. */
    [self endComposition];
    [self forwardCommittedDocument:textView.text ?: @""];
}

- (void)deleteBackward {
    /* 프록시는 포커스를 얻을 때 빈 문서에서 시작한다. HMJE 필드에 기존 값이
     * 있어도 빈 프록시의 backspace를 잃지 않도록 명시적으로 전달한다. */
    if (self.text.length == 0) {
        legacy_key_push(51ULL, YES);
        legacy_key_push(51ULL, NO);
        return;
    }
    [super deleteBackward];
}

@end

static HmjeTextInputProxy *g_text_input_proxy = nil;

static void hmje_ios_apply_keyboard_safe_area(void) {
    if (g_root_controller == nil || g_window == nil) return;
    CGFloat baseBottom = g_window.safeAreaInsets.bottom;
    CGFloat extraBottom = 0.0;
    if (!g_keyboard_safe_area_ignored && g_keyboard_overlap > baseBottom) {
        extraBottom = g_keyboard_overlap - baseBottom;
    }
    UIEdgeInsets extra = g_root_controller.additionalSafeAreaInsets;
    if (fabs(extra.bottom - extraBottom) < 0.5) return;
    extra.bottom = extraBottom;
    g_root_controller.additionalSafeAreaInsets = extra;
    [g_root_controller.view setNeedsLayout];
    [g_root_controller.view layoutIfNeeded];
}

static i64 legacy_input_pop(void) {
    os_unfair_lock_lock(&g_legacy_lock);
    i64 value = hmje_legacy_input_ring_pop(&g_legacy_input);
    os_unfair_lock_unlock(&g_legacy_lock);
    return value;
}

static u64 legacy_pointer_event(u64 kind, u64 x, u64 y, BOOL down) {
    u64 value = (kind << HMJE_INPUT_KIND_SHIFT)
              | (x & HMJE_INPUT_POINTER_COORD_MASK)
              | ((y & HMJE_INPUT_POINTER_COORD_MASK) << HMJE_INPUT_POINTER_Y_SHIFT);
    if (down) value |= HMJE_INPUT_POINTER_PRESSED_FLAG;
    return value;
}

static void legacy_primary_emit(void *context, uint32_t event, uint64_t x, uint64_t y) {
    (void)context;
    if (event == HMJE_LEGACY_PRIMARY_EVENT_MOVE) {
        legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, x, y, NO));
    } else {
        legacy_input_push(legacy_pointer_event(
            HMJE_INPUT_KIND_POINTER_BUTTON,
            x,
            y,
            event == HMJE_LEGACY_PRIMARY_EVENT_BUTTON_DOWN
        ));
    }
}

/* hmje가 방출한 진입점. ios_sim.py가 LLVM IR 텍스트에서 @main → @hmje_app_main 으로
 * 미리 바꿔놓는다(lower_llvm.py는 안 건드림) — 그래서 이 프로세스의 진짜 main()과
 * 심볼이 충돌하지 않는다. */
extern int hmje_app_main(void);

/* ---- 표면 재구성: scene 연결 시점의 screen.bounds 스냅샷은 기기 회전·실기기
 * 세로/가로 불일치에서 실제 뷰 크기와 어긋난다(터치는 뷰 좌표 × 배율, 프레임버퍼는
 * 옛 크기 → 좌표가 통째로 안 맞는 증상). macOS 쪽과 같은 계약으로 맞춘다:
 * 뷰 레이아웃이 바뀔 때 백버퍼·drawable을 실제 크기로 재구성하고, poll이
 * "앱이 아는 기하 ≠ 실제 기하"인 동안 RESIZE 이벤트를 합성한다(유실 불가). */
static u64 g_acked_width = 0;
static u64 g_acked_height = 0;
static u64 g_acked_scale = 0;
static u64 g_acked_safe_top = 0;
static u64 g_acked_safe_right = 0;
static u64 g_acked_safe_bottom = 0;
static u64 g_acked_safe_left = 0;

static void reconfigure_surface_for_size(CGSize boundsSize, CGFloat screenScale) {
    if (screenScale < 1.0) {
        screenScale = 1.0;
    }
    u64 new_scale = (u64)screenScale;
    if (new_scale < 1) {
        new_scale = 1;
    }
    u64 new_width = (u64)(boundsSize.width * screenScale);
    u64 new_height = (u64)(boundsSize.height * screenScale);
    if (new_width == 0 || new_height == 0) {
        return;
    }
    if (new_width == g_width && new_height == g_height && new_scale == g_scale) {
        return;
    }
    u64 new_pitch = new_width * 4;
    /* The canonical CPU canvas must never alias an in-flight Metal upload.
     * SoftwarePresentPool owns per-frame staging buffers, so resize replaces
     * only this CPU allocation regardless of whether Metal is available. */
    unsigned char *new_pixels = calloc(1, (size_t)(new_pitch * new_height));
    if (new_pixels == NULL) {
        return; /* 할당 실패 — 이전 표면 유지 */
    }
    free(g_backbuffer);
    g_backbuffer = new_pixels;
    g_width = new_width;
    g_height = new_height;
    g_pitch = new_pitch;
    g_scale = new_scale;
    if (g_metal_layer != nil) {
        g_metal_layer.contentsScale = screenScale;
        g_metal_layer.drawableSize = CGSizeMake((CGFloat)new_width, (CGFloat)new_height);
    }
}

static i64 surface_resize_event_if_needed(void) {
    if (g_width == 0 || g_height == 0) {
        return 0;
    }
    /* UIKit may publish the final notch/home-indicator insets after the first
     * non-zero layout without changing bounds. Treat that as a geometry
     * revision too, so the portable layout gets another chance to read the
     * framebuffer-info v3 safe-area fields on real devices. */
    if (g_width == g_acked_width && g_height == g_acked_height && g_scale == g_acked_scale
        && g_safe_top == g_acked_safe_top && g_safe_right == g_acked_safe_right
        && g_safe_bottom == g_acked_safe_bottom && g_safe_left == g_acked_safe_left) {
        return 0;
    }
    g_acked_width = g_width;
    g_acked_height = g_height;
    g_acked_scale = g_scale;
    g_acked_safe_top = g_safe_top;
    g_acked_safe_right = g_safe_right;
    g_acked_safe_bottom = g_safe_bottom;
    g_acked_safe_left = g_safe_left;
    u64 event = (HMJE_INPUT_KIND_SURFACE_RESIZE << HMJE_INPUT_KIND_SHIFT)
        | ((g_scale & HMJE_INPUT_SURFACE_SCALE_MASK) << HMJE_INPUT_SURFACE_SCALE_SHIFT)
        | ((g_height & HMJE_INPUT_SURFACE_SIZE_MASK) << HMJE_INPUT_SURFACE_HEIGHT_SHIFT)
        | (g_width & HMJE_INPUT_SURFACE_SIZE_MASK);
    return (i64)event;
}

/* ---- 커스텀 콘텐츠 뷰: macOS 쪽 HmjeContentView와 그리기 로직은 동일(Core Graphics는
 * 두 플랫폼이 같은 API) — CGContextDrawImage가 항상 "현재 좌표계 기준 bottom-up"으로 그리는
 * 건 AppKit/UIKit 공통 특성이라 동일한 translate+scale(-1) 보정이 여기서도 그대로 필요하다. */

@interface HmjeContentView : UIView
@end

@implementation HmjeContentView

- (void)updateSafeAreaCache {
    UIEdgeInsets insets = self.safeAreaInsets;
    CGFloat scale = (CGFloat)(g_scale > 0 ? g_scale : 1);
    g_safe_top = (u64)llround(insets.top * scale);
    g_safe_right = (u64)llround(insets.right * scale);
    g_safe_bottom = (u64)llround(insets.bottom * scale);
    g_safe_left = (u64)llround(insets.left * scale);
}

/* 회전·초기 레이아웃·창 크기 변화를 전부 여기서 받는다(메인 스레드).
 * blit/present도 메인 스레드로 마셜링되므로 재구성과 직렬화된다. */
- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat scale = self.contentScaleFactor;
    if (self.window != nil && self.window.windowScene != nil) {
        scale = self.window.windowScene.screen.scale;
        self.contentScaleFactor = scale;
    }
    reconfigure_surface_for_size(self.bounds.size, scale);
    [self updateSafeAreaCache];
}

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self updateSafeAreaCache];
}

/* coalesced/predicted 표본은 같은 접촉의 세부 표본이지만 UIKit이 별개의
 * UITouch 인스턴스로 준다. 객체 주소를 그대로 포인터 식별자로 쓰면 한 획의
 * 이동 표본마다 id가 갈려, 캡처 매칭과 소비자의 동시 입력 필터가 모두
 * 깨진다(획이 눌림-뗌 두 점짜리 직선으로 남는다). 좌표·압력을 읽는 표본과
 * 식별자를 주는 원본 접촉을 분리한다. */
- (void)pushTouch:(UITouch *)touch
         identity:(UITouch *)identity
            event:(UIEvent *)event
            phase:(uint64_t)phase
            flags:(uint64_t)sampleFlags {
    CGPoint point = [touch locationInView:self];
    int64_t x = (int64_t)llround(point.x * (CGFloat)g_scale);
    int64_t y = (int64_t)llround(point.y * (CGFloat)g_scale);
    hmje_pointer_record record;
    memset(&record, 0, sizeof(record));
    record.phase = phase;
    record.pointer_id = (uint64_t)(uintptr_t)identity;
    record.x = x;
    record.y = y;
    record.time_us = (uint64_t)(touch.timestamp * 1000000.0);
    record.flags = sampleFlags;
    record.changed_button = 0u;

    switch (touch.type) {
        case UITouchTypePencil:
            record.tool = HMJE_POINTER_TOOL_PEN;
            break;
        case UITouchTypeDirect:
            record.tool = HMJE_POINTER_TOOL_TOUCH;
            break;
        case UITouchTypeIndirectPointer:
            record.tool = HMJE_POINTER_TOOL_MOUSE;
            break;
        default:
            record.tool = HMJE_POINTER_TOOL_UNKNOWN;
            break;
    }
    if (touch.maximumPossibleForce > 0.0) {
        double pressure = touch.force / touch.maximumPossibleForce;
        if (pressure < 0.0) pressure = 0.0;
        if (pressure > 1.0) pressure = 1.0;
        record.pressure = (uint64_t)llround(pressure * 65535.0);
        record.flags |= HMJE_POINTER_CAP_PRESSURE;
    }
    if (touch.type == UITouchTypePencil) {
        double altitude = touch.altitudeAngle / (M_PI / 2.0);
        if (altitude < 0.0) altitude = 0.0;
        if (altitude > 1.0) altitude = 1.0;
        record.altitude = (uint64_t)llround(altitude * 65535.0);
        double azimuth = [touch azimuthAngleInView:self];
        if (azimuth < 0.0) azimuth += 2.0 * M_PI;
        record.azimuth = (uint64_t)llround((azimuth / (2.0 * M_PI)) * 65535.0);
        record.flags |= HMJE_POINTER_CAP_ALTITUDE | HMJE_POINTER_CAP_AZIMUTH;
    }
    if (@available(iOS 13.4, *)) {
        record.buttons = (uint64_t)event.buttonMask;
        record.flags |= HMJE_POINTER_CAP_BUTTONS;
    }
    hmje_pointer_ring_push(&g_pointer_samples, record);

    /* 예측 표본은 실제 접촉보다 미래 좌표다. 고해상도 포인터 소비자만 받으며,
     * 버튼/스크롤 히트테스트가 쓰는 레거시 좌표에는 절대 섞지 않는다. */
    if ((sampleFlags & HMJE_POINTER_FLAG_PREDICTED) != 0u) return;

    /* 레거시 단일 포인터 경로도 같은 접촉 기준이어야 한다 — 세부 표본 주소로
     * 비교하면 coalesced 이동이 주 접촉과 안 맞아 전부 버려진다. 상세 queue는
     * 위에서 모든 접촉을 이미 보존했고, 아래 state machine은 legacy projection만
     * latest-DOWN-wins로 정규화한다. 새 DOWN은 allTouches의 stale 판정과 무관하게
     * 이전 sequence를 UP으로 닫으므로 영구 pressed primary가 남을 수 없다. */
    uintptr_t identity_address = (uintptr_t)identity;
    u64 ux = x < 0 ? 0u : (u64)x;
    u64 uy = y < 0 ? 0u : (u64)y;
    BOOL primary_is_active = legacy_touch_identity_is_active(
        event, g_legacy_primary.pointer_id
    );
    hmje_legacy_primary_route(
        &g_legacy_primary,
        identity_address,
        phase,
        ux,
        uy,
        primary_is_active,
        legacy_primary_emit,
        NULL
    );
}

- (void)feedTouches:(NSSet<UITouch *> *)touches
              event:(UIEvent *)event
              phase:(uint64_t)phase {
    for (UITouch *touch in touches) {
        if (phase == HMJE_POINTER_PHASE_MOVE) {
            NSArray<UITouch *> *coalesced = [event coalescedTouchesForTouch:touch];
            if (coalesced.count > 0) {
                for (UITouch *sample in coalesced) {
                    [self pushTouch:sample
                           identity:touch
                              event:event
                              phase:phase
                              flags:HMJE_POINTER_FLAG_COALESCED];
                }
            } else {
                [self pushTouch:touch identity:touch event:event phase:phase flags:0u];
            }
            for (UITouch *sample in [event predictedTouchesForTouch:touch]) {
                [self pushTouch:sample
                       identity:touch
                          event:event
                          phase:phase
                          flags:HMJE_POINTER_FLAG_PREDICTED];
            }
        } else {
            [self pushTouch:touch identity:touch event:event phase:phase flags:0u];
        }
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self feedTouches:touches event:event phase:HMJE_POINTER_PHASE_DOWN];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self feedTouches:touches event:event phase:HMJE_POINTER_PHASE_MOVE];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self feedTouches:touches event:event phase:HMJE_POINTER_PHASE_UP];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self feedTouches:touches event:event phase:HMJE_POINTER_PHASE_CANCEL];
}

+ (Class)layerClass {
    if (g_device != nil) {
        return [CAMetalLayer class];
    }
    return [super layerClass];
}

- (void)drawRect:(CGRect)dirtyRect {
    (void)dirtyRect;
    if (g_device != nil) {
        return; /* Metal 경로는 present가 drawable에 직접 blit한다. */
    }
    if (g_backbuffer == NULL || g_width == 0 || g_height == 0) {
        return;
    }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, g_backbuffer, (size_t)(g_pitch * g_height), NULL);
    CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
    CGImageRef image = CGImageCreate((size_t)g_width, (size_t)g_height, 8, 32, (size_t)g_pitch,
                                      colorSpace, bitmapInfo, provider, NULL, false, kCGRenderingIntentDefault);
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, (CGFloat)g_height);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)g_width, (CGFloat)g_height), image);
    CGContextRestoreGState(ctx);
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);
}

@end

/* ---- Scene lifecycle ------------------------------------------------------------------
 * 이 SDK(iOS 27.0)는 UIScene 생명주기를 요구한다(TN3187). Info.plist에
 * UIApplicationSceneManifest를 선언하고 UISceneDelegate로 창을 만든다. */





@interface HmjeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end

@implementation HmjeSceneDelegate
/* Notify the frame loop from a BeforeWaiting observer, not from the display
 * link callback itself: touches for this refresh are delivered as run-loop
 * sources in the same pass, and their order against the display link timer is
 * not guaranteed.  Signalling once the pass is about to sleep means every
 * touch UIKit handed us this refresh is already in the pointer queue when the
 * HMJE loop drains input, so a drag consumes exactly one refresh's samples per
 * frame instead of alternating 0/2. */
static atomic_bool g_display_link_pending = ATOMIC_VAR_INIT(false);
static CFRunLoopObserverRef g_display_link_observer = NULL;

static void hmje_ios_frame_observer(CFRunLoopObserverRef observer,
                                    CFRunLoopActivity activity, void *info) {
    (void)observer; (void)activity; (void)info;
    if (atomic_exchange_explicit(&g_display_link_pending, false, memory_order_acq_rel)) {
        hmje_async_notify_frame();
    }
}

- (void)hmjeDisplayLinkFired:(CADisplayLink *)link {
    (void)link;
    atomic_store_explicit(&g_display_link_pending, true, memory_order_release);
    if (g_display_link_observer == NULL) {
        g_display_link_observer = CFRunLoopObserverCreate(
            kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true, LONG_MAX,
            hmje_ios_frame_observer, NULL);
        CFRunLoopAddObserver(CFRunLoopGetMain(), g_display_link_observer, kCFRunLoopCommonModes);
    }
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    /* CADisplayLink keeps the application runnable in the background unless
     * explicitly paused.  Preserve it so the same scene can resume cheaply. */
    g_display_link.paused = YES;
    atomic_store_explicit(&g_display_link_pending, false, memory_order_release);
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
    (void)scene;
    g_display_link.paused = NO;
    hmje_async_notify_frame();
}

- (void)sceneDidDisconnect:(UIScene *)scene {
    (void)scene;
    [g_display_link invalidate];
    g_display_link = nil;
    atomic_store_explicit(&g_display_link_pending, false, memory_order_release);
}


- (void)keyboardFrameChanged:(NSNotification *)notification {
    if (g_window == nil) return;
    if ([notification.name isEqualToString:UIKeyboardWillHideNotification]) {
        g_keyboard_overlap = 0.0;
        hmje_ios_apply_keyboard_safe_area();
        return;
    }
    NSValue *frameValue = notification.userInfo[UIKeyboardFrameEndUserInfoKey];
    if (frameValue == nil) return;
    CGRect keyboardInWindow = [g_window convertRect:frameValue.CGRectValue fromWindow:nil];
    CGRect intersection = CGRectIntersection(g_window.bounds, keyboardInWindow);
    g_keyboard_overlap = CGRectIsNull(intersection) ? 0.0 : intersection.size.height;
    hmje_ios_apply_keyboard_safe_area();
}

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions {
    (void)session;
    /* 주소로 차갑게 시작했으면 그 주소가 연결 옵션으로 온다(scene:openURLContexts:는 오지 않는다). */
    app_open_url_contexts(connectionOptions.URLContexts);
    if (![scene isKindOfClass:[UIWindowScene class]]) {
        return;
    }
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    CGRect screenBounds = windowScene.screen.bounds;
    CGFloat screenScale = windowScene.screen.scale;
    if (screenScale < 1.0) {
        screenScale = 1.0;
    }
    g_device = MTLCreateSystemDefaultDevice();
    if (g_device != nil) {
        g_queue = [g_device newCommandQueue];
        if (g_queue == nil) {
            g_device = nil;
        }
    }
    g_scale = (u64)screenScale;
    if (g_scale < 1) {
        g_scale = 1;
    }
    /* UIKit bounds는 point 단위지만 HMJE framebuffer 계약은 물리 픽셀 단위다. */
    g_width = (u64)(screenBounds.size.width * screenScale);
    g_height = (u64)(screenBounds.size.height * screenScale);
    g_pitch = g_width * 4;
    size_t frame_bytes = (size_t)(g_pitch * g_height);
    g_backbuffer = calloc(1, frame_bytes);
    if (g_backbuffer == NULL) {
        g_device = nil;
        g_queue = nil;
    }
    if (g_device != nil) {
        g_software_present_pool = [[HmjeSoftwarePresentPool alloc]
            initWithDevice:g_device
                     width:(NSUInteger)g_width
                    height:(NSUInteger)g_height
               bytesPerRow:(NSUInteger)g_pitch];
        if (g_software_present_pool == nil) {
            g_device = nil;
            g_queue = nil;
        }
    }

    UIWindow *window = [[UIWindow alloc] initWithWindowScene:windowScene];
    HmjeContentView *view = [[HmjeContentView alloc] initWithFrame:screenBounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.contentScaleFactor = screenScale;
    view.backgroundColor = [UIColor blackColor];
    view.multipleTouchEnabled = YES;
    if (g_device != nil) {
        CAMetalLayer *layer = (CAMetalLayer *)view.layer;
        layer.device = g_device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.opaque = YES;
        /* nextDrawable may still pace up to Core Animation's own bounded
         * timeout, but it must never wait forever after drawable starvation. */
        layer.allowsNextDrawableTimeout = YES;
        layer.maximumDrawableCount = HMJE_IOS_METAL_MAX_FRAMES_IN_FLIGHT;
        layer.contentsScale = screenScale;
        layer.drawableSize = CGSizeMake((CGFloat)g_width, (CGFloat)g_height);
        g_metal_layer = layer;
    }

    UIViewController *rootController = [[UIViewController alloc] init];
    rootController.view = view;
    window.rootViewController = rootController;

    self.window = window;
    g_window = window;
    g_view = view;
    g_root_controller = rootController;
    if (g_display_link == nil) {
        g_display_link = [CADisplayLink displayLinkWithTarget:self
                                                     selector:@selector(hmjeDisplayLinkFired:)];
        if (@available(iOS 15.0, *)) {
            /* Prefer 60Hz even on ProMotion: the HMJE loop presents one frame per
             * wake, and a stable 60 beats an alternating 120/60 cadence. */
            g_display_link.preferredFrameRateRange = CAFrameRateRangeMake(60.0f, 120.0f, 60.0f);
        }
        [g_display_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }

    HmjeTextInputProxy *textInputProxy = [[HmjeTextInputProxy alloc]
        initWithFrame:CGRectMake(-1000.0, -1000.0, 1.0, 1.0)
        textContainer:nil];
    [view addSubview:textInputProxy];
    g_text_input_proxy = textInputProxy;

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(keyboardFrameChanged:)
                                                 name:UIKeyboardWillChangeFrameNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(keyboardFrameChanged:)
                                                 name:UIKeyboardWillHideNotification
                                               object:nil];

    [window makeKeyAndVisible];

    dispatch_semaphore_signal(g_launch_sem);
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts {
    (void)scene;
    app_open_url_contexts(URLContexts);
}

@end

@interface HmjeAppDelegate : NSObject <UIApplicationDelegate>
@end

@implementation HmjeAppDelegate

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default Configuration"
                                                                    sessionRole:connectingSceneSession.role];
    config.delegateClass = [HmjeSceneDelegate class];
    return config;
}

@end

static void ensure_window(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_semaphore_wait(g_launch_sem, DISPATCH_TIME_FOREVER);
    });
}

static HmjeSoftwarePresentPool *software_present_pool_for_surface(
    NSUInteger width,
    NSUInteger height,
    NSUInteger bytes_per_row
) {
    if (g_device == nil || width == 0 || height == 0 || bytes_per_row == 0) return nil;
    @synchronized([HmjeSoftwarePresentPool class]) {
        if (g_software_present_pool != nil &&
            [g_software_present_pool matchesWidth:width
                                           height:height
                                      bytesPerRow:bytes_per_row]) {
            return g_software_present_pool;
        }
        HmjeSoftwarePresentPool *replacement = [[HmjeSoftwarePresentPool alloc]
            initWithDevice:g_device
                     width:width
                    height:height
               bytesPerRow:bytes_per_row];
        if (replacement == nil) return nil;
        /* In-flight completions retain the previous pool.  Replacing this
         * strong root therefore cannot invalidate an old slot mid-blit. */
        g_software_present_pool = replacement;
        return replacement;
    }
}

/* ---- 검증용 디버그 이스케이프 해치: macOS posix_host_window.m과 동일한 아이디어.
 * HMJE_POSIX_DUMP_FRAME이 설정되어 있으면 매 present마다 합성된 백버퍼를 PNG로 덮어쓴다.
 * 시뮬레이터 스크린샷(xcrun simctl io screenshot)이 1차 증거고, 이건 로컬 반복 확인용. */
static void dump_frame_debug(void) {
    const char *path = getenv("HMJE_POSIX_DUMP_FRAME");
    if (path == NULL || g_backbuffer == NULL) {
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

/* ---- posix_host_shim.c 가 부르는 4개 함수 (macOS와 동일 시그니처/시맨틱) ------------- */

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
    if (out_length >= 88) {
        out[6] = g_scale;
        out[7] = g_safe_top;
        out[8] = g_safe_right;
        out[9] = g_safe_bottom;
        out[10] = g_safe_left;
        return 88;
    }
    if (out_length >= 56) {
        out[6] = g_scale;
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

i64 hmje_posix_framebuffer_present(i64 rects_address, i64 count) {
    ensure_window();
    (void)rects_address;
    BOOL has_new_frame = count > 0;
    u64 software_epoch = 0;
    if (has_new_frame) {
        software_epoch = hmje_ios_present_next_epoch();
    } else if (!hmje_ios_present_pending_epoch(&software_epoch)) return 0;
    __block i64 present_result = 1;
    if (g_device == nil) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                [g_view setNeedsDisplay];
                [g_view.layer displayIfNeeded];
            }
        });
        hmje_ios_present_set_pending_for_epoch(software_epoch, NO);
    } else {
        /* Keep one immutable surface-generation snapshot through upload and
         * encoding.  A future UIKit resize callback may swap the globals on
         * the main queue, but it must not mix a new pitch with this old slot. */
        u64 frame_width_u64 = g_width;
        u64 frame_height_u64 = g_height;
        u64 frame_pitch_u64 = g_pitch;
        unsigned char *frame_pixels = g_backbuffer;
        if (frame_pitch_u64 == 0 || frame_height_u64 == 0 ||
            frame_height_u64 > (u64)(NSUIntegerMax / (NSUInteger)frame_pitch_u64) ||
            frame_pixels == NULL) {
            return HMJE_POSIX_ENOSYS_LOCAL;
        }
        NSUInteger frame_width = (NSUInteger)frame_width_u64;
        NSUInteger frame_height = (NSUInteger)frame_height_u64;
        NSUInteger frame_pitch = (NSUInteger)frame_pitch_u64;
        NSUInteger frame_bytes = (NSUInteger)(frame_pitch_u64 * frame_height_u64);
        HmjeSoftwarePresentPool *pool = software_present_pool_for_surface(
            frame_width, frame_height, frame_pitch
        );
        if (pool == nil) return HMJE_POSIX_ENOSYS_LOCAL;

        /* The HMJE thread is the sole writer of g_backbuffer.  Taking a pool
         * slot and snapshotting here lets the call return after commit while
         * later dirty blits continue against the independent CPU canvas. */
        int slot = [pool takeSlot];
        if (slot < 0) {
            /* The complete CPU frame is already safe in g_backbuffer.  Report
             * the new frame as accepted rather than making transient GPU
             * backpressure fatal; an idle flush will retry without blocking. */
            hmje_ios_present_set_pending_for_epoch(software_epoch, YES);
            return has_new_frame ? 1 : 0;
        }
        id<MTLBuffer> staging = [pool bufferAtSlot:slot];
        id<MTLTexture> software_texture = [pool softwareTexture];
        if (staging == nil || staging.length < frame_bytes ||
            software_texture == nil || software_texture.width != frame_width ||
            software_texture.height != frame_height) {
            [pool releaseSlot:slot];
            return HMJE_POSIX_ENOSYS_LOCAL;
        }
        memcpy(staging.contents, frame_pixels, frame_bytes);

        __block BOOL submitted = NO;
        dispatch_sync(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                id<CAMetalDrawable> drawable = [g_metal_layer nextDrawable];
                if (drawable == nil) return;
                id<MTLTexture> texture = drawable.texture;
                NSUInteger copy_width = MIN(frame_width, texture.width);
                NSUInteger copy_height = MIN(frame_height, texture.height);
                if (copy_width == 0 || copy_height == 0) return;
                id<MTLCommandBuffer> command_buffer = [g_queue commandBuffer];
                if (command_buffer == nil) {
                    present_result = HMJE_POSIX_ENOSYS_LOCAL;
                    return;
                }
                id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
                if (encoder == nil) {
                    present_result = HMJE_POSIX_ENOSYS_LOCAL;
                    return;
                }
                /* Finish the comparatively slow shared-buffer upload entirely
                 * offscreen.  Only the completed private image participates in
                 * the short drawable copy immediately before presentation. */
                [encoder copyFromBuffer:staging
                           sourceOffset:0
                      sourceBytesPerRow:frame_pitch
                    sourceBytesPerImage:0
                             sourceSize:MTLSizeMake(frame_width, frame_height, 1)
                              toTexture:software_texture
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
                [encoder copyFromTexture:software_texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                               toTexture:texture
                        destinationSlice:0
                        destinationLevel:0
                       destinationOrigin:MTLOriginMake(0, 0, 0)];
                [encoder endEncoding];
                [command_buffer presentDrawable:drawable];
                /* Explicitly capture both objects.  Metal normally retains
                 * referenced resources, but this also makes lifetime correct
                 * if the current pool is replaced by a later surface size. */
                id<MTLBuffer> retained_staging = staging;
                [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                    (void)retained_staging;
                    if (completed.status != MTLCommandBufferStatusCompleted) {
                        hmje_ios_present_set_pending_for_epoch(software_epoch, YES);
                    }
                    [pool releaseSlot:slot];
                }];
                /* Clear before commit so a very fast failed completion cannot
                 * set pending=true and then be overwritten by the submitter. */
                hmje_ios_present_set_pending_for_epoch(software_epoch, NO);
                [command_buffer commit];
                submitted = YES;
            }
        });
        if (!submitted) {
            [pool releaseSlot:slot];
            hmje_ios_present_set_pending_for_epoch(software_epoch, YES);
            return has_new_frame ? 1 : 0;
        }
    }
    dump_frame_debug();
    return present_result;
}

i64 hmje_posix_input_poll(void) {
    ensure_window();
    i64 resize_event = surface_resize_event_if_needed();
    if (resize_event != 0) {
        return resize_event;
    }
    return legacy_input_pop();
}

i64 hmje_posix_input_poll2(i64 out_address, i64 out_length) {
    ensure_window();
    return hmje_pointer_ring_pop(&g_pointer_samples, out_address, out_length);
}

#include "text_input_control.h"
static u64 g_text_input_client, g_text_input_flags;
static i64 hmje_ios_text_input_apply(u64 client, u64 flags, BOOL requireSecure) {
    ensure_window();
    __block i64 result = -38;
    void (^apply)(void) = ^{
        if (g_text_input_proxy == nil) return;
        BOOL secure = (flags & HMJE_TEXT_INPUT_SECURE) != 0;
        /* Email fields: iOS autocorrect happily rewrites "semtle" to "settle" and capitalizes the
         * first letter, which surfaced as intermittent 401s on login. */
        BOOL email = !secure && (flags & HMJE_TEXT_INPUT_EMAIL) != 0;
        BOOL plain = !secure && !email;
        BOOL traitsChanged = ((flags ^ g_text_input_flags) & (HMJE_TEXT_INPUT_SECURE | HMJE_TEXT_INPUT_MULTILINE | HMJE_TEXT_INPUT_EMAIL)) != 0;
        BOOL clientChanged = client != g_text_input_client || ((flags ^ g_text_input_flags) & HMJE_TEXT_INPUT_ACTIVE) || traitsChanged;
        g_text_input_client = client;
        g_text_input_flags = flags;
        g_text_input_proxy.secureTextEntry = secure;
        g_text_input_proxy.textContentType = secure ? UITextContentTypePassword : (email ? UITextContentTypeEmailAddress : nil);
        g_text_input_proxy.keyboardType = email ? UIKeyboardTypeEmailAddress : UIKeyboardTypeDefault;
        g_text_input_proxy.autocapitalizationType = plain ? UITextAutocapitalizationTypeSentences : UITextAutocapitalizationTypeNone;
        g_text_input_proxy.autocorrectionType = plain ? UITextAutocorrectionTypeDefault : UITextAutocorrectionTypeNo;
        g_text_input_proxy.spellCheckingType = plain ? UITextSpellCheckingTypeDefault : UITextSpellCheckingTypeNo;
        g_text_input_proxy.smartDashesType = plain ? UITextSmartDashesTypeDefault : UITextSmartDashesTypeNo;
        g_text_input_proxy.smartQuotesType = plain ? UITextSmartQuotesTypeDefault : UITextSmartQuotesTypeNo;
        g_text_input_proxy.smartInsertDeleteType = plain ? UITextSmartInsertDeleteTypeDefault : UITextSmartInsertDeleteTypeNo;
        g_text_input_proxy.returnKeyType = (secure && !(flags & HMJE_TEXT_INPUT_MULTILINE)) ? UIReturnKeyDone : UIReturnKeyDefault;
        g_keyboard_safe_area_ignored = (flags & HMJE_TEXT_INPUT_IGNORE_SAFE_AREA) != 0;
        hmje_ios_apply_keyboard_safe_area();
        if (clientChanged) [g_text_input_proxy resetDocument];
        if (flags & HMJE_TEXT_INPUT_ACTIVE) {
            if (traitsChanged && g_text_input_proxy.isFirstResponder) [g_text_input_proxy reloadInputViews];
            BOOL active = [g_text_input_proxy becomeFirstResponder] || g_text_input_proxy.isFirstResponder;
            result = active && (!requireSecure || g_text_input_proxy.isSecureTextEntry) ? 1 : -38;
        } else {
            [g_text_input_proxy resignFirstResponder];
            [g_text_input_proxy resetDocument];
            result = 1;
        }
    };
    if ([NSThread isMainThread]) apply(); else dispatch_sync(dispatch_get_main_queue(), apply);
    return result;
}
i64 hmje_posix_text_input_control(i64 request) {
    return hmje_ios_text_input_apply(((u64)request) >> 2, ((u64)request) & 3, NO);
}
i64 hmje_posix_text_input_control2(i64 version, i64 client, i64 flags) {
    int valid = hmje_text_input_validate((u64)version, (u64)client, (u64)flags);
    if (valid) return valid;
    return hmje_ios_text_input_apply((u64)client, (u64)flags, ((u64)flags & HMJE_TEXT_INPUT_SECURE) != 0);
}

i64 hmje_posix_ui_clipboard(i64 operation, i64 address, i64 length) {
    __block i64 result = -22;
    void (^apply)(void) = ^{
        UIPasteboard *pasteboard = [UIPasteboard generalPasteboard];
        if (operation == 0) {
            NSString *text = pasteboard.string ?: @"";
            NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
            NSUInteger count = utf8.length;
            if (address != 0 && length >= 0 && (u64)length >= (u64)count && count > 0)
                memcpy((void *)(uintptr_t)(u64)address, utf8.bytes, count);
            result = (i64)count;
            return;
        }
        if (operation == 1) {
            if (length < 0 || (address == 0 && length != 0)) { result = -22; return; }
            NSString *text = [[NSString alloc] initWithBytes:(const void *)(uintptr_t)(u64)address
                                                      length:(NSUInteger)length
                                                    encoding:NSUTF8StringEncoding];
            if (text == nil) { result = -22; return; }
            pasteboard.string = text;
            result = length;
        }
    };
    if ([NSThread isMainThread]) apply(); else dispatch_sync(dispatch_get_main_queue(), apply);
    return result;
}

/* Direct Metal command backend. Kept in a renderer-only include so UIKit
 * lifecycle, input recovery and bundle-font handling remain isolated here. */
#include "posix_ios_metal_renderer.inc"
static BOOL hmje_ax_input_is_first_responder(void) { return g_text_input_proxy.isFirstResponder; }
#include "posix_ios_accessibility.inc"

/* ---- posix_host_shim.c의 weak 자원 훅 오버라이드 ---------------------------------------
 * 표준 자원 API와 옛 `/system/fonts/{name}` graph walk가 같은 이름을 여기서 앱 번들에서
 * 찾는다. 프로젝트 빌드는 모듈 자원을 논리 경로 그대로 번들 뿌리에 복사하고
 * (compiler/Sources/제품/iOS번들.hmje `iOS자원복사`), 단일 파일 개발 빌드는 폰트를
 * `assets/fonts/`에 넣는다. 둘 다 받는다. 옛 배치(폰트가 번들 뿌리에 바로 있음)도
 * `fonts/{name}`에 한해 받는다. */
static i64 hmje_ios_open_bundle_path(NSString *path) {
    if (path == nil) {
        return -ENOENT;
    }
    int fd = open([path fileSystemRepresentation], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -ENOENT;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -ENOENT;
    }
    return (i64)fd;
}

i64 hmje_posix_open_resource_file(const char *name, size_t name_len) {
    @autoreleasepool {
        NSString *logical = [[NSString alloc] initWithBytes:name length:name_len encoding:NSUTF8StringEncoding];
        NSString *root = [[NSBundle mainBundle] resourcePath];
        if (logical == nil || root == nil) {
            return -ENOENT;
        }
        NSFileManager *files = [NSFileManager defaultManager];
        NSArray<NSString *> *candidates = @[
            [root stringByAppendingPathComponent:logical],
            [[root stringByAppendingPathComponent:@"assets"] stringByAppendingPathComponent:logical],
        ];
        if ([logical hasPrefix:@"fonts/"]) {
            candidates = [candidates arrayByAddingObject:
                [root stringByAppendingPathComponent:[logical substringFromIndex:6]]];
        }
        for (NSString *candidate in candidates) {
            if ([files fileExistsAtPath:candidate]) {
                return hmje_ios_open_bundle_path(candidate);
            }
        }
        return -ENOENT;
    }
}

/* ---- 진짜 프로세스 진입점 -------------------------------------------------------------
 * hmje 앱 로직(=hmje_app_main, 원래 @main)은 별도 스레드에서 돈다. 그 안에서 SYS_EXIT이
 * 불리면(화면을 표시하는 함수는 60초 루프 뒤 항상 종료를 호출한다) exit()가 프로세스
 * 전체를 끝낸다 — 어느 스레드에서 부르든 exit()의 계약은 그대로다. */

static void *hmje_thread_entry(void *arg) {
    (void)arg;
    hmje_app_main();
    return NULL;
}

int main(int argc, char *argv[]) {
    /* HMJE 파일 API는 "assets/..." 같은 상대 경로를 그대로 연다. iOS 프로세스의
     * 시작 cwd는 "/"라 전부 실패하므로(글꼴·자산이 통째로 빈다) 번들 리소스
     * 루트를 작업 디렉터리로 삼는다 — 패키징이 assets/·fonts/를 여기에 둔다. */
    @autoreleasepool {
        const char *resources = [[[NSBundle mainBundle] resourcePath] fileSystemRepresentation];
        if (resources != NULL) {
            chdir(resources);
        }
        /* 범용 파일 API의 데이터 루트는 쓰기 가능한 앱 데이터 위치를 사용한다.
         * 금고의 앱데이터경로()도 같은 위치를 사용하여 절대 경로를 공유한다. */

        NSURL *support = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
                                                                inDomain:NSUserDomainMask
                                                       appropriateForURL:nil
                                                                  create:YES
                                                                   error:nil];
        if (support != nil) {
            NSURL *root = [support URLByAppendingPathComponent:@"hmje-data" isDirectory:YES];
            [[NSFileManager defaultManager] createDirectoryAtURL:root withIntermediateDirectories:YES
                                                      attributes:@{NSFilePosixPermissions: @0700} error:nil];
            extern void hmje_posix_set_file_data_dir(const char *path);
            hmje_posix_set_file_data_dir([[root path] fileSystemRepresentation]);
        }
    }
    g_launch_sem = dispatch_semaphore_create(0);

    pthread_t thread;
    /* The HMJE frame loop is the app's UI thread in all but name: give it
     * USER_INTERACTIVE QoS so the scheduler treats its vsync-paced wakes and
     * frame work like the main thread's (default QoS on iOS lands it on
     * efficiency cores with slower wakes, which shows up as scroll judder). */
    pthread_attr_t attr;
    int status = pthread_attr_init(&attr);
    if (status != 0) {
        fprintf(stderr, "HMJE app thread attributes failed: %s (%d)\n", strerror(status), status);
        return EXIT_FAILURE;
    }
    status = pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    if (status == 0) status = pthread_create(&thread, &attr, hmje_thread_entry, NULL);
    pthread_attr_destroy(&attr);
    if (status != 0) {
        fprintf(stderr, "HMJE app thread creation failed: %s (%d)\n", strerror(status), status);
        return EXIT_FAILURE;
    }
    pthread_detach(thread);

    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([HmjeAppDelegate class]));
    }
}

/* UIKit trait reads belong to the main queue; HMJE itself runs on its worker. */
i64 hmje_posix_system_theme(void) {
    __block i64 result = 1;
    void (^readTheme)(void) = ^{
        UITraitCollection *traits = g_view != nil ? g_view.traitCollection : g_window.traitCollection;
        if (traits == nil) traits = UIScreen.mainScreen.traitCollection;
        result = traits.userInterfaceStyle == UIUserInterfaceStyleDark ? 2 : 1;
    };
    if ([NSThread isMainThread]) readTheme();
    else dispatch_sync(dispatch_get_main_queue(), readTheme);
    return result;
}

/* HmjeUI의 앱 상태 유산: scene 활성 상태를 앱 전체의 가장 앞선 상태로 접는다. */
i64 hmje_posix_ui_window(i64 operation, i64 argument1, i64 argument2) {
    (void)argument1; (void)argument2;
    if (operation != 14) return -38;
    __block i64 result = 3;
    void (^readState)(void) = ^{
        UIApplication *app = UIApplication.sharedApplication;
        for (UIScene *scene in app.connectedScenes) {
            if (scene.activationState == UISceneActivationStateForegroundActive) { result = 1; return; }
            if (scene.activationState == UISceneActivationStateForegroundInactive) result = 2;
        }
        if (app.connectedScenes.count == 0) {
            if (app.applicationState == UIApplicationStateActive) result = 1;
            else if (app.applicationState == UIApplicationStateInactive) result = 2;
        }
    };
    if ([NSThread isMainThread]) readState();
    else dispatch_sync(dispatch_get_main_queue(), readState);
    return result;
}
