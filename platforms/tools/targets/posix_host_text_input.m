/*
 * posix_host_text_input.m — posix-host(macOS) 문자 입력 파이프라인.
 *
 * posix_host_window.m의 키 이벤트 모니터가 받은 NSEvent를 전부 이 유닛으로 넘긴다.
 * 여기서 두 종류의 입력 이벤트(library/kernel/api/syscall.hmje 계약)로 변환해
 * hmje_posix_input_push_event(윈도우 유닛의 입력 링)로 큐잉한다:
 *
 *   - INPUT_EVENT_KIND_KEY (상위 8비트 = 0): 물리 키 down/up. 하위 8비트 = macOS 가상
 *     keyCode, 비트 8 = 눌림, 비트 16~19 = 수정자(shift/ctrl/alt/cmd). 화살표·엔터·
 *     백스페이스·단축키·게임 키 상태표는 전부 이 스트림.
 *   - INPUT_EVENT_KIND_TEXT (상위 8비트 = 4): 확정(committed) 유니코드 코드포인트
 *     (비트 0~20). NSTextInputClient를 채택한 숨은 뷰가 first responder가 되어
 *     macOS IME가 한글 조합을 대신 해주고, insertText:로 확정된 문자열만 코드포인트
 *     단위로 큐잉한다.
 *   - INPUT_EVENT_KIND_COMPOSITION (상위 8비트 = 8): 조합 중(marked text) 문자열.
 *     setMarkedText:마다 코드포인트들 + 끝 이벤트(캐럿)로 큐잉해 HMJE가 캐럿 자리에
 *     밑줄과 함께 그린다. 문자 없는 끝 이벤트 = 조합 끝(취소). 확정은 KIND_TEXT다.
 *     HMJE는 그린 조합 사각형을 SYS_TEXT_INPUT_CONTROL2(2, …)로 되돌려 주고,
 *     firstRectForCharacterRange:가 그 사각형으로 후보창 위치를 답한다.
 *
 * 이벤트 순서: keyDown 하나가 텍스트를 확정하면 KIND_TEXT를 먼저, 그 keyDown의
 * KIND_KEY down을 나중에 큐잉한다. (엔터로 한글 조합을 확정하는 경우 앱이
 * "확정 문자 → 엔터" 순서로 보게 하기 위함.)
 *
 * cmd/ctrl이 눌린 keyDown은 IME로 보내지 않는다(단축키는 텍스트가 아니다).
 * 단축키 글자는 입력 소스와 무관하게 정한다(2벌식에서는 Cmd+Z의
 * charactersIgnoringModifiers가 "ㅋ"다 — posix_host_keymap.h).
 *
 * 입력 문맥은 이 숨은 뷰가 first responder일 때만 활성이다. 콘텐츠 뷰가 클릭으로
 * first responder를 가져가면 비활성 문맥의 handleEvent:가 아무 호출 없이 키를
 * 삼켜 한글이 전혀 안 들어가고, 활성/비활성이 오가면 IME가 조합을 자모 단위로
 * 확정한다. 그래서 키를 보내기 전에 매번 이 뷰를 first responder로 되돌리고,
 * 클릭(포인터 누름)·포커스 변경 때는 조합을 명시적으로 끝낸다.
 *
 * 검증용 이스케이프 해치(HMJE_POSIX_DUMP_FRAME과 같은 정신): HMJE_POSIX_SYNTH_TEXT
 * 환경변수가 설정되어 있으면 창 attach 직후 그 UTF-8 문자열을 insertText: 경로로
 * 흘려 KIND_TEXT 이벤트를 만든다 — CGEvent 합성(TCC 권한 필요) 없이 IME 확정 경로
 * (insertText: → 코드포인트 분해 → 큐잉 → SYS_INPUT_POLL)를 그대로 검증한다.
 */

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_host_keymap.h"
#include "posix_host_ime_policy.h"
#include "text_input_control.h"

typedef long long i64;
typedef unsigned long long u64;

/* posix_host_window.m가 노출하는 입력 링 push. */
extern void hmje_posix_input_push_event(u64 event);
NSEvent *hmje_posix_text_input_handle_event(NSEvent *event);

/* library/kernel/api/syscall.hmje 계약과 값이 같아야 하는 로컬 상수(별도 번역 단위). */
#define HMJE_INPUT_KIND_SHIFT 56
#define HMJE_INPUT_KIND_KEY 0ULL
#define HMJE_INPUT_KIND_TEXT 4ULL
#define HMJE_INPUT_KIND_EDIT_COMMAND 7ULL
#define HMJE_INPUT_PRESSED_FLAG 256ULL
#define HMJE_INPUT_MOD_SHIFT 65536ULL
#define HMJE_INPUT_MOD_CTRL 131072ULL
#define HMJE_INPUT_MOD_ALT 262144ULL
#define HMJE_INPUT_MOD_SUPER 524288ULL
#define HMJE_INPUT_MOD_PRIMARY 1048576ULL
#define HMJE_INPUT_KIND_COMPOSITION 8ULL
#define HMJE_INPUT_COMPOSITION_END_FLAG 4194304ULL
#define HMJE_INPUT_COMPOSITION_CARET_MASK 65535ULL
#define HMJE_INPUT_TEXT_CODEPOINT_MASK 2097151ULL
#define HMJE_INPUT_TEXT_CONTINUATION_FLAG 2097152ULL
#define HMJE_EDIT_SELECT_ALL 1ULL
#define HMJE_EDIT_COPY 2ULL
#define HMJE_EDIT_CUT 3ULL
#define HMJE_EDIT_PASTE 4ULL
#define HMJE_EDIT_UNDO 5ULL
#define HMJE_EDIT_REDO 6ULL

/* ---- 창별 IME 상태 ---------------------------------------------------------
 * 창마다 숨은 NSTextInputClient 뷰·조합 사각형·보안 입력 상태가 따로 있다.
 * posix_host_window.m이 창 객체에 이 상태를 들고 있고, 지금 창(g_win)의 것을
 * hmje_posix_window_text_state()로 준다. 아래 별칭 덕분에 기존 코드는 그대로
 * "지금 창"의 상태를 읽고 쓴다. IME가 뷰 메서드를 부를 때는 그 뷰의 창으로
 * 경로를 돌린다(HMJE_TEXT_ROUTE). */
@class HmjeTextInputView;

@interface HmjeTextInputState : NSObject {
@public
    HmjeTextInputView *view;
    /* The rectangle HMJE drew the composition (or the caret) in, in content-view
     * points with a top-left origin: SYS_TEXT_INPUT_CONTROL2(2, x|y<<32, w|h<<32). */
    NSRect caret_rect;
    BOOL caret_rect_valid;
    /* HMJE owns the masked document; this proxy only routes committed input. */
    BOOL secure_text_input;
}
@end

@implementation HmjeTextInputState
@end

extern id hmje_posix_window_text_state(void);
extern void hmje_posix_window_set_text_state(id state);
extern void *hmje_posix_window_route_nswindow(NSWindow *window);
extern void hmje_posix_window_route_restore(void *previous);

static HmjeTextInputState *g_null_text_state = nil;

static HmjeTextInputState *text_state(void) {
    HmjeTextInputState *state = hmje_posix_window_text_state();
    if (state != nil) return state;
    if (g_null_text_state == nil) g_null_text_state = [[HmjeTextInputState alloc] init];
    return g_null_text_state;
}

#define g_text_view (text_state()->view)
#define g_caret_rect (text_state()->caret_rect)
#define g_caret_rect_valid (text_state()->caret_rect_valid)
#define g_secure_text_input (text_state()->secure_text_input)
#define HMJE_TEXT_ROUTE(view) void *hmje_text_route_previous = hmje_posix_window_route_nswindow([(view) window])
#define HMJE_TEXT_UNROUTE() hmje_posix_window_route_restore(hmje_text_route_previous)

/* While the proxy ends a composition itself, the IME may answer
 * discardMarkedText with its own commit. That text was already queued. */
static int g_ending_composition = 0;

static void push_edit_command(u64 command) {
    if (g_secure_text_input && (command == HMJE_EDIT_COPY || command == HMJE_EDIT_CUT))
        return;
    hmje_posix_input_push_event((HMJE_INPUT_KIND_EDIT_COMMAND << HMJE_INPUT_KIND_SHIFT)
                                | (command & 255ULL));
}

static u64 modifier_bits(NSEventModifierFlags flags) {
    u64 bits = 0;
    if (flags & NSEventModifierFlagShift) {
        bits |= HMJE_INPUT_MOD_SHIFT;
    }
    if (flags & NSEventModifierFlagControl) {
        bits |= HMJE_INPUT_MOD_CTRL;
    }
    if (flags & NSEventModifierFlagOption) {
        bits |= HMJE_INPUT_MOD_ALT;
    }
    if (flags & NSEventModifierFlagCommand) {
        /* Command is this platform's primary shortcut modifier (`명령`). */
        bits |= HMJE_INPUT_MOD_SUPER | HMJE_INPUT_MOD_PRIMARY;
    }
    return bits;
}

static void push_key_event(unsigned short keyCode, BOOL pressed, NSEventModifierFlags flags, BOOL compositionReturn) {
    keyCode = hmje_posix_normalize_key_code(keyCode);
    u64 event = (u64)(keyCode & 0xFF);
    if (pressed) {
        event |= HMJE_INPUT_PRESSED_FLAG;
    }
    event |= modifier_bits(flags);
    if (compositionReturn) event |= (1ULL << 21);
    /* KIND_KEY == 0이라 상위 8비트는 이미 0이지만 계약 형태를 명시해둔다. */
    event |= (HMJE_INPUT_KIND_KEY << HMJE_INPUT_KIND_SHIFT);
    hmje_posix_input_push_event(event);
}

static void push_text_string(NSString *string) {
    if (string == nil || [string length] == 0) {
        return;
    }
    /* UTF-32LE로 변환해 코드포인트 단위로 순회한다(서러게이트 쌍 처리 포함). */
    NSData *utf32 = [string dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (utf32 == nil) {
        return;
    }
    const unsigned char *bytes = (const unsigned char *)[utf32 bytes];
    NSUInteger count = [utf32 length] / 4;
    NSUInteger emitted = 0;
    for (NSUInteger i = 0; i < count; i++) {
        u64 codepoint = (u64)bytes[i * 4]
                      | ((u64)bytes[i * 4 + 1] << 8)
                      | ((u64)bytes[i * 4 + 2] << 16)
                      | ((u64)bytes[i * 4 + 3] << 24);
        if (codepoint == 0) {
            continue; /* BOM 없는 UTF-32LE라 0은 나올 일 없지만 방어 */
        }
        u64 event = (HMJE_INPUT_KIND_TEXT << HMJE_INPUT_KIND_SHIFT)
                  | (codepoint & HMJE_INPUT_TEXT_CODEPOINT_MASK);
        if (emitted != 0) event |= HMJE_INPUT_TEXT_CONTINUATION_FLAG;
        hmje_posix_input_push_event(event);
        emitted++;
    }
}

/* One composition update: its code points, then an END record carrying the
 * caret (in code points) inside the composed string. No code points = the
 * composition ended without a commit (cancel, focus change). */
static void push_composition(NSString *string, NSUInteger caret_utf16) {
    NSUInteger caret = 0;
    NSUInteger length = [string length];
    NSUInteger index = 0;
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
        if (index < caret_utf16) caret++;
        hmje_posix_input_push_event((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT)
                                    | (codepoint & HMJE_INPUT_TEXT_CODEPOINT_MASK));
        index += width;
    }
    if (caret > HMJE_INPUT_COMPOSITION_CARET_MASK) caret = HMJE_INPUT_COMPOSITION_CARET_MASK;
    hmje_posix_input_push_event((HMJE_INPUT_KIND_COMPOSITION << HMJE_INPUT_KIND_SHIFT)
                                | HMJE_INPUT_COMPOSITION_END_FLAG | (u64)caret);
}

static void push_composition_clear(void) {
    push_composition(@"", 0);
}


/* ---- NSTextInputClient 뷰: IME(한글 조합 등)의 클라이언트 ------------------- */

@interface HmjeTextInputView : NSView <NSTextInputClient>
@property(nonatomic, strong) NSMutableAttributedString *markedText;
/* The IME's selection inside markedText (UTF-16), e.g. the caret or the
 * clause being converted. */
@property(nonatomic, assign) NSRange markedSelection;
@property(nonatomic, assign) BOOL inputCommittedText;
@property(nonatomic, assign) BOOL inputChangedMark;
@property(nonatomic, assign) BOOL inputRequestedCommand;
@end

@implementation HmjeTextInputView

- (BOOL)acceptsFirstResponder {
    return YES;
}

/* -- 입력(커밋/조합) -------------------------------------------------------- */

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    HMJE_TEXT_ROUTE(self);
    [self routedInsertText:string replacementRange:replacementRange];
    HMJE_TEXT_UNROUTE();
}

- (void)routedInsertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (g_ending_composition) {
        self.markedText = nil;
        return;
    }
    NSString *plain = nil;
    if ([string isKindOfClass:[NSAttributedString class]]) {
        plain = [(NSAttributedString *)string string];
    } else if ([string isKindOfClass:[NSString class]]) {
        plain = (NSString *)string;
    }
    BOOL wasMarked = self.markedText != nil;
    self.markedText = nil; /* 커밋되면 조합 상태는 끝 */
    self.inputCommittedText = YES;
    /* Committed text replaces the composition for HMJE. An empty commit
     * still has to take the composition off the screen. */
    if ([plain length] > 0) push_text_string(plain);
    else if (wasMarked) push_composition_clear();
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    HMJE_TEXT_ROUTE(self);
    [self routedSetMarkedText:string selectedRange:selectedRange replacementRange:replacementRange];
    HMJE_TEXT_UNROUTE();
}

- (void)routedSetMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (g_ending_composition) return;
    BOOL wasMarked = self.markedText != nil;
    self.inputChangedMark = YES;
    if ([string isKindOfClass:[NSAttributedString class]]) {
        self.markedText = [[NSMutableAttributedString alloc] initWithAttributedString:(NSAttributedString *)string];
    } else if ([string isKindOfClass:[NSString class]]) {
        self.markedText = [[NSMutableAttributedString alloc] initWithString:(NSString *)string];
    } else {
        self.markedText = nil;
    }
    if (self.markedText != nil && [self.markedText length] == 0) {
        self.markedText = nil;
    }
    if (self.markedText == nil) {
        self.markedSelection = NSMakeRange(0, 0);
        if (wasMarked) push_composition_clear();
        return;
    }
    NSUInteger length = [self.markedText length];
    NSUInteger location = selectedRange.location == NSNotFound ? length : MIN(selectedRange.location, length);
    self.markedSelection = NSMakeRange(location, MIN(selectedRange.length, length - location));
    /* The caret follows the IME's selection (its end, for a converting clause). */
    push_composition([self.markedText string], NSMaxRange(self.markedSelection));
}

- (void)unmarkText {
    HMJE_TEXT_ROUTE(self);
    [self routedUnmarkText];
    HMJE_TEXT_UNROUTE();
}

- (void)routedUnmarkText {
    if (g_ending_composition) {
        self.markedText = nil;
        return;
    }
    /* IME가 조합을 그냥 확정시키라는 뜻 — 남아 있는 조합분을 커밋으로 취급한다. */
    NSString *pending = self.markedText ? [self.markedText string] : nil;
    self.markedText = nil;
    self.inputChangedMark = YES;
    self.inputCommittedText = YES;
    if ([pending length] > 0) push_text_string(pending);
}

- (void)doCommandBySelector:(SEL)selector {
    /* 엔터/백스페이스/화살표 등은 KIND_KEY 스트림이 이미 전달한다 — 여기서는 무시.
     * (super로 넘기면 시스템 beep가 난다.) */
    (void)selector;
    self.inputRequestedCommand = YES;
}

/* Menu clicks and keyboard equivalents converge on the same semantic input
 * events. HMJE, not this zero-sized proxy, owns the document and selection. */
- (void)routedEditCommand:(u64)command {
    HMJE_TEXT_ROUTE(self);
    push_edit_command(command);
    HMJE_TEXT_UNROUTE();
}
- (void)selectAll:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_SELECT_ALL]; }
- (void)copy:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_COPY]; }
- (void)cut:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_CUT]; }
- (void)paste:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_PASTE]; }
- (void)undo:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_UNDO]; }
- (void)redo:(id)sender { (void)sender; [self routedEditCommand:HMJE_EDIT_REDO]; }

/* -- 조합 상태 조회 --------------------------------------------------------- */

- (BOOL)hasMarkedText {
    return self.markedText != nil;
}

- (NSRange)markedRange {
    if (self.markedText == nil) {
        return NSMakeRange(NSNotFound, 0);
    }
    return NSMakeRange(0, [self.markedText length]);
}

/* The proxy document is exactly the marked text (markedRange starts at 0),
 * so the selection is the one the IME set inside it; with no composition the
 * empty document has its caret at 0. HMJE's committed document is not mirrored. */
- (NSRange)selectedRange {
    if (self.markedText == nil) return NSMakeRange(0, 0);
    return self.markedSelection;
}

/* The proxy document is exactly the marked text (markedRange starts at 0).
 * The Korean IME reads the syllable back before extending it; answer with
 * what it marked instead of nil. HMJE's committed document is not mirrored. */
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    NSUInteger length = self.markedText ? [self.markedText length] : 0;
    if (range.location == NSNotFound || range.location >= length) {
        if (actualRange != NULL) *actualRange = NSMakeRange(NSNotFound, 0);
        return nil;
    }
    NSRange clipped = NSMakeRange(range.location, MIN(range.length, length - range.location));
    if (actualRange != NULL) *actualRange = clipped;
    return [self.markedText attributedSubstringFromRange:clipped];
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}

/* The candidate window goes under the rectangle HMJE drew the composition
 * in. A sub-range of the marked text (a clause) gets its share of that width. */
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    HMJE_TEXT_ROUTE(self);
    NSRect result = [self routedFirstRectForCharacterRange:range actualRange:actualRange];
    HMJE_TEXT_UNROUTE();
    return result;
}

- (NSRect)routedFirstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    NSUInteger length = self.markedText ? [self.markedText length] : 0;
    NSRange actual = NSMakeRange(0, 0);
    if (length > 0 && range.location != NSNotFound) {
        NSUInteger location = MIN(range.location, length);
        actual = NSMakeRange(location, MIN(range.length, length - location));
    }
    if (actualRange != NULL) *actualRange = actual;
    NSWindow *window = [self window];
    if (window == nil) {
        return NSZeroRect;
    }
    NSView *content = [window contentView];
    NSRect rect = g_caret_rect_valid ? g_caret_rect : NSMakeRect(0, 0, 1, 16);
    if (g_caret_rect_valid && length > 0) {
        CGFloat unit = rect.size.width / (CGFloat)length;
        rect.origin.x += unit * (CGFloat)actual.location;
        rect.size.width = actual.length > 0 ? unit * (CGFloat)actual.length : 0;
    }
    rect = [content convertRect:rect toView:nil];
    return [window convertRectToScreen:rect];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

@end

/* ---- posix_host_window.m가 부르는 2개 함수 --------------------------------- */

static BOOL g_synth_text_delivered = NO;
static u64 g_synth_text_poll_count = 0;
static u64 g_ime_suppressed_keyups[4] = {0, 0, 0, 0};

/* Finish the IME composition from the host side. commit=YES queues the marked
 * text first (pointer press: it belongs to the caret the user is leaving);
 * commit=NO drops it (focus already moved in HMJE). In both cases the input
 * context is told, otherwise the IME keeps composing a syllable the client
 * no longer reports and restarts it jamo by jamo on the next key. */
static void end_composition(BOOL commit) {
    if (g_text_view == nil || g_text_view.markedText == nil) return;
    NSString *pending = [[g_text_view.markedText string] copy];
    g_text_view.markedText = nil;
    /* The committed text replaces the composition; a drop must clear it. */
    if (commit) push_text_string(pending);
    else push_composition_clear();
    g_ending_composition++;
    [[g_text_view inputContext] discardMarkedText];
    g_ending_composition--;
}

/* posix_host_window.m calls this before it queues a pointer press. */
void hmje_posix_text_input_pointer_down(void) {
    end_composition(YES);
}

/* The hidden proxy must stay first responder: only then is its input context
 * the active one. A click on the content view, a closed panel or AppKit focus
 * restoration can move it; take it back before routing a key. */
static void ensure_first_responder(void) {
    NSWindow *window = [g_text_view window];
    if (window != nil && [window firstResponder] != g_text_view) {
        [window makeFirstResponder:g_text_view];
    }
}

int hmje_posix_text_input_is_attached(void) {
    return hmje_posix_window_text_state() != nil;
}

/* The HmjeUI text editor already masks secure fields and owns selection,
 * editing, and clipboard policy. The macOS input proxy acknowledges secure
 * focus without creating an NSSecureTextField or storing the document. */
i64 hmje_posix_text_input_control2(i64 version, i64 client, i64 flags) {
    if (version == HMJE_TEXT_INPUT_CARET_RECT) {
        NSRect rect = NSMakeRect((CGFloat)((u64)client & 0xffffffffULL), (CGFloat)((u64)client >> 32),
                                 (CGFloat)((u64)flags & 0xffffffffULL), (CGFloat)((u64)flags >> 32));
        if (!g_caret_rect_valid || !NSEqualRects(rect, g_caret_rect)) {
            g_caret_rect = rect;
            g_caret_rect_valid = YES;
            if (g_text_view != nil) [[g_text_view inputContext] invalidateCharacterCoordinates];
            if (getenv("HMJE_POSIX_SELFTEST_POINTER") != NULL)
                fprintf(stderr, "posix-host-text-input: caret-rect %.0f,%.0f %.0fx%.0f\n",
                        rect.origin.x, rect.origin.y, rect.size.width, rect.size.height);
        }
        return 1;
    }
    int valid = hmje_text_input_validate((u64)version, (u64)client, (u64)flags);
    if (valid) return valid;
    g_secure_text_input = client != 0 && (((u64)flags & HMJE_TEXT_INPUT_SECURE) != 0);
    end_composition(NO);
    return 1;
}

/* Ordinary/blur requests use the original packed syscall. They must drop
 * secure mode so a later ordinary field regains its normal edit commands. */
i64 hmje_posix_text_input_control(i64 request) {
    (void)request;
    g_secure_text_input = NO;
    end_composition(NO);
    return 1;
}

static void set_suppressed_keyup(unsigned short keyCode) {
    if (keyCode < 256) g_ime_suppressed_keyups[keyCode / 64] |= 1ULL << (keyCode % 64);
}

static BOOL take_suppressed_keyup(unsigned short keyCode) {
    if (keyCode >= 256) return NO;
    u64 mask = 1ULL << (keyCode % 64);
    BOOL found = (g_ime_suppressed_keyups[keyCode / 64] & mask) != 0;
    g_ime_suppressed_keyups[keyCode / 64] &= ~mask;
    return found;
}

static void synth_text_debug(void) {
    if (g_synth_text_delivered) {
        return;
    }
    const char *synth = getenv("HMJE_POSIX_SYNTH_TEXT");
    if (synth == NULL || synth[0] == '\0' || g_text_view == nil) {
        return;
    }
    NSString *text = [NSString stringWithUTF8String:synth];
    if (text != nil) {
        /* IME가 조합 확정 시 부르는 것과 정확히 같은 진입점을 쓴다. */
        [g_text_view insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
        g_synth_text_delivered = YES;
    }
}

/* 포인터 셀프테스트가 텍스트입력을 먼저 포커스한 뒤 확정 문자를 넣을 수 있게
 * 입력 poll 횟수로 지연한다. 환경변수가 없으면 기존처럼 attach 즉시 넣는다. */
void hmje_posix_text_input_selftest_tick(void) {
    const char *at = getenv("HMJE_POSIX_SYNTH_TEXT_AT");
    if (at == NULL || at[0] == '\0' || g_synth_text_delivered) {
        return;
    }
    g_synth_text_poll_count++;
    if (g_synth_text_poll_count >= strtoull(at, NULL, 10)) {
        synth_text_debug();
    }
}

/* HMJE_POSIX_SELFTEST_POINTER actions for text input (posix_host_window.m):
 *   ime:<utf8>      the IME marks <utf8> (setMarkedText, like each 2-Set step)
 *   commit:<utf8>   the IME commits <utf8> (insertText)
 *   key:<code>[,<mods>]  a physical key down/up through the key monitor;
 *                   <code> is the macOS virtual keyCode, <mods> any of
 *                   c(ommand) s(hift) o(ption) t(control). Shortcut letters
 *                   carry no characters, as under a non-Latin input source.
 * Returns 1 when the action was one of these. */
static char shortcut_letter(NSEvent *event);

int hmje_posix_text_input_selftest_action(const char *action) {
    if (g_text_view == nil || action == NULL) return 0;
    if (strncmp(action, "ime:", 4) == 0 || strncmp(action, "commit:", 7) == 0) {
        BOOL commit = action[0] == 'c';
        NSString *text = [NSString stringWithUTF8String:action + (commit ? 7 : 4)];
        if (text == nil) return 1;
        if (commit) {
            [g_text_view insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
        } else {
            [g_text_view setMarkedText:text selectedRange:NSMakeRange([text length], 0)
                      replacementRange:NSMakeRange(NSNotFound, 0)];
        }
        return 1;
    }
    unsigned int code = 0;
    char mods[8] = {0};
    int fields = sscanf(action, "key:%u,%7[csot]", &code, mods);
    if (fields < 1) return 0;
    NSEventModifierFlags flags = 0;
    if (strchr(mods, 'c')) flags |= NSEventModifierFlagCommand;
    if (strchr(mods, 's')) flags |= NSEventModifierFlagShift;
    if (strchr(mods, 'o')) flags |= NSEventModifierFlagOption;
    if (strchr(mods, 't')) flags |= NSEventModifierFlagControl;
    NSString *characters = @"";
    if ((flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0) {
        switch (code) {
            case 36: characters = @"\r"; break;
            case 48: characters = @"\t"; break;
            case 51: characters = @"\x7f"; break;
            case 53: characters = @"\x1b"; break;
            case 123: characters = [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey]; break;
            case 124: characters = [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey]; break;
            case 125: characters = [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey]; break;
            case 126: characters = [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey]; break;
            case 120: characters = [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey]; break;
            default: break;
        }
    }
    NSWindow *window = [g_text_view window];
    for (int down = 1; down >= 0; down--) {
        NSEvent *event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                          location:NSZeroPoint modifierFlags:flags
                                         timestamp:[[NSProcessInfo processInfo] systemUptime]
                                      windowNumber:[window windowNumber] context:nil
                                        characters:characters charactersIgnoringModifiers:characters
                                         isARepeat:NO keyCode:(unsigned short)code];
        NSEvent *unhandled = hmje_posix_text_input_handle_event(event);
        /* ⌘W·⌘Q·⌘M·⌘H처럼 AppKit 메뉴 몫으로 돌려준 단축키는 실제 키처럼
         * 메뉴의 key equivalent로 보낸다. */
        if (down && unhandled != nil) {
            /* 합성 키는 앱이 활성이 아니어도(터미널에서 돌린 시험) 같은 결과가
             * 나야 한다. 메뉴 action의 대상(키 창)을 직접 부른다. */
            char letter = shortcut_letter(unhandled);
            if (letter == 'w') [window performClose:nil];      /* 창 메뉴 "창 닫기" */
            else if (letter == 'q') [NSApp terminate:nil];     /* 앱 메뉴 "종료" */
            else (void)[[NSApp mainMenu] performKeyEquivalent:unhandled];
        }
    }
    return 1;
}

/* posix_host_window.m이 창을 만들 때 그 창을 지금 창으로 두고 부른다. */
void hmje_posix_text_input_attach(NSWindow *window) {
    if (window == nil || hmje_posix_window_text_state() != nil) {
        return;
    }
    hmje_posix_window_set_text_state([[HmjeTextInputState alloc] init]);
    @autoreleasepool {
        /* 화면에 아무것도 안 그리는 0x0 뷰 — first responder + NSTextInputClient 역할만. */
        HmjeTextInputView *view = [[HmjeTextInputView alloc] initWithFrame:NSZeroRect];
        [[window contentView] addSubview:view];
        [window makeFirstResponder:view];
        g_text_view = view;
        if (getenv("HMJE_POSIX_SYNTH_TEXT_AT") == NULL) {
            synth_text_debug();
        }
    }
}

static char shortcut_letter(NSEvent *event) {
    NSString *ignoring = [event charactersIgnoringModifiers];
    unsigned int first = [ignoring length] > 0 ? [ignoring characterAtIndex:0] : 0;
    return hmje_posix_shortcut_letter([event keyCode], first);
}

/* 키 모니터 진입점. 이벤트를 소비하려면 nil을 반환한다(기존 동작 유지). */
NSEvent *hmje_posix_text_input_handle_event(NSEvent *event) {
    if (event == nil) {
        return nil;
    }
    NSEventType type = [event type];
    if (type == NSEventTypeFlagsChanged) {
        /* 수정자 키 자체의 down/up. 해당 keyCode의 플래그가 서 있으면 down. */
        NSEventModifierFlags flags = [event modifierFlags];
        unsigned short keyCode = [event keyCode];
        NSEventModifierFlags probe = 0;
        switch (keyCode) {
            case 56: case 60: probe = NSEventModifierFlagShift; break;   /* shift */
            case 59: case 62: probe = NSEventModifierFlagControl; break; /* control */
            case 58: case 61: probe = NSEventModifierFlagOption; break;  /* option */
            case 55: case 54: probe = NSEventModifierFlagCommand; break; /* command */
            case 57: probe = NSEventModifierFlagCapsLock; break;         /* caps lock */
            default: probe = 0; break;
        }
        BOOL pressed = probe != 0 && (flags & probe) != 0;
        push_key_event(keyCode, pressed, flags, NO);
        return nil;
    }
    if (type == NSEventTypeKeyUp) {
        if (take_suppressed_keyup([event keyCode])) return nil;
        if (([event modifierFlags] & NSEventModifierFlagCommand) != 0) {
            char letter = shortcut_letter(event);
            if (letter == 'a' || letter == 'c' || letter == 'x' || letter == 'v' ||
                letter == 'z' || letter == 'q' || letter == 'w' || letter == 'm' ||
                letter == 'h') return nil;
        }
        push_key_event([event keyCode], NO, [event modifierFlags], NO);
        return nil;
    }
    if (type != NSEventTypeKeyDown) {
        return event; /* 관심 없는 이벤트는 그대로 통과 */
    }

    NSEventModifierFlags flags = [event modifierFlags];
    if ((flags & NSEventModifierFlagCommand) != 0) {
        char letter = shortcut_letter(event);
        u64 command = 0;
        if (letter == 'a') command = HMJE_EDIT_SELECT_ALL;
        else if (letter == 'c') command = HMJE_EDIT_COPY;
        else if (letter == 'x') command = HMJE_EDIT_CUT;
        else if (letter == 'v') command = HMJE_EDIT_PASTE;
        else if (letter == 'z')
            command = (flags & NSEventModifierFlagShift) ? HMJE_EDIT_REDO : HMJE_EDIT_UNDO;
        if (command != 0) {
            /* A shortcut ends the syllable the same way a click does. */
            end_composition(YES);
            push_edit_command(command);
            return nil;
        }
        /* App/window lifecycle belongs to the standard AppKit menus. Do not
         * also expose it as an application key hook. */
        if (letter == 'q' || letter == 'w' || letter == 'm' || letter == 'h')
            return event;
    }
    BOOL compositionReturn = NO;
    BOOL shortcut = (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    if (shortcut) {
        /* Cmd+S and friends go to HMJE as keys; commit the syllable first so
         * a save sees it. */
        end_composition(YES);
    }
    if (!shortcut && g_text_view != nil) {
        ensure_first_responder();
        /* IME(현재 입력 소스)로 라우팅 — 한글이면 조합, 확정 시 insertText:가 불려
         * KIND_TEXT가 이 keyDown의 KIND_KEY보다 먼저 큐잉된다. */
        NSTextInputContext *context = [g_text_view inputContext];
        if (context != nil) {
            BOOL wasMarked = [g_text_view hasMarkedText];
            g_text_view.inputCommittedText = NO;
            g_text_view.inputChangedMark = NO;
            g_text_view.inputRequestedCommand = NO;
            BOOL handled = [context handleEvent:event];
            compositionReturn = wasMarked &&
                ([event keyCode] == 36 || [event keyCode] == 76);
            /* Composition owns its keys, while ordinary AppKit command
             * selectors (arrows, Backspace) still belong to HMJE. */
            if (!hmje_posix_ime_forward_key(wasMarked, [g_text_view hasMarkedText],
                                            g_text_view.inputCommittedText,
                                            g_text_view.inputChangedMark,
                                            handled, g_text_view.inputRequestedCommand)) {
                set_suppressed_keyup([event keyCode]);
                return nil;
            }
        }
    }
    push_key_event([event keyCode], YES, flags, compositionReturn);
    return nil;
}
