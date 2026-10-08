/* Ordinary UIKit accessibility for the shared HmjeUI semantic tree.
 * Called by the HMJE UI executor; UIKit objects are touched only on main.
 * No network/debug server, borrowed HMJE pointers or synthetic pointer taps.
 * Wire ABI: syscall 109, op 0 support, 1 snapshot (18 u64/node), 2 action poll.
 */
#define HMJE_AX_NODE_WORDS 18
#define HMJE_AX_MAX_NODES 8192
#define HMJE_AX_MAX_TEXT_BYTES (1024 * 1024)

typedef struct { u64 identifier, action; } HmjeAXAction;
static HmjeAXAction g_ax_actions[64];
static unsigned g_ax_read, g_ax_write;
static os_unfair_lock g_ax_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSNumber *, id> *g_ax_elements;
static NSMutableSet<NSNumber *> *g_ax_current;

static BOOL hmje_ax_enqueue(u64 identifier, u64 action) {
    os_unfair_lock_lock(&g_ax_lock);
    if (![g_ax_current containsObject:@(identifier)]) { os_unfair_lock_unlock(&g_ax_lock); return NO; }
    unsigned next = (g_ax_write + 1) % 64;
    if (next == g_ax_read) { os_unfair_lock_unlock(&g_ax_lock); return NO; }
    g_ax_actions[g_ax_write] = (HmjeAXAction){identifier, action};
    g_ax_write = next;
    os_unfair_lock_unlock(&g_ax_lock);
    return YES;
}

@interface HmjeAccessibilityElement : UIAccessibilityElement
@property(nonatomic, assign) u64 semanticIdentifier;
@property(nonatomic, assign) u64 actions;
@end
@implementation HmjeAccessibilityElement
- (BOOL)accessibilityActivate {
    if (self.actions & 1) return hmje_ax_enqueue(self.semanticIdentifier, 1);
    if (self.actions & 2) return hmje_ax_enqueue(self.semanticIdentifier, 2);
    return NO;
}
- (void)accessibilityIncrement {
    if (self.actions & 4) hmje_ax_enqueue(self.semanticIdentifier, 4);
}
- (void)accessibilityDecrement {
    if (self.actions & 8) hmje_ax_enqueue(self.semanticIdentifier, 8);
}
@end

/* Native text field semantics without a second editor or second hit target.
 * HMJE remains the visible renderer/editor. Activation focuses its existing
 * text session and the existing IME proxy continues receiving keyboard input.
 * Keeping this actual UIKit view in the hierarchy gives XCTest/VoiceOver the
 * public text-field role; no private accessibility trait bits are used. */
@interface HmjeAccessibilityTextField : UITextField
@property(nonatomic, assign) BOOL semanticFocused;
@property(nonatomic, assign) u64 semanticIdentifier;
@property(nonatomic, assign) u64 actions;
@end
@implementation HmjeAccessibilityTextField
- (BOOL)canBecomeFirstResponder { return NO; }
/* Delegate keyboard-focus reporting to the real IME responder. */
- (BOOL)isFirstResponder { return self.semanticFocused && hmje_ax_input_is_first_responder(); }
- (BOOL)accessibilityActivate {
    return (self.actions & 2) && hmje_ax_enqueue(self.semanticIdentifier, 2);
}
- (void)drawRect:(CGRect)rect { (void)rect; }
@end

static void hmje_ax_on_main(dispatch_block_t block) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

static NSString *hmje_ax_text(u64 address, u64 length) {
    if (length == 0) return @"";
    if (address == 0 || length > HMJE_AX_MAX_TEXT_BYTES) return nil;
    return [[NSString alloc] initWithBytes:(const void *)(uintptr_t)address
                                   length:(NSUInteger)length
                                 encoding:NSUTF8StringEncoding];
}

i64 hmje_posix_ui_accessibility(i64 operation, i64 address, i64 length) {
    if (operation == 0) return 1;
    if (operation == 2) {
        if (!address || length < (i64)sizeof(HmjeAXAction)) return -22;
        os_unfair_lock_lock(&g_ax_lock);
        if (g_ax_read == g_ax_write) { os_unfair_lock_unlock(&g_ax_lock); return 0; }
        HmjeAXAction action = g_ax_actions[g_ax_read];
        g_ax_read = (g_ax_read + 1) % 64;
        os_unfair_lock_unlock(&g_ax_lock);
        memcpy((void *)(uintptr_t)(u64)address, &action, sizeof(action));
        return (i64)sizeof(action);
    }
    if (operation != 1) return -38;
    const size_t stride = HMJE_AX_NODE_WORDS * sizeof(u64);
    if (length < 0 || (u64)length % stride || (u64)length / stride > HMJE_AX_MAX_NODES)
        return -22;
    if (length && !address) return -22;
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
            if (!label || !value) return -22;
            [keys addObject:@(r[0])];
            [snapshot addObject:@{@"id": @(r[0]), @"role": @(r[2]), @"state": @(r[3]),
                @"actions": @(r[4]), @"label": label, @"value": value,
                @"frame": [NSValue valueWithCGRect:CGRectMake((i64)r[6], (i64)r[7], r[8], r[9])]}];
        }
        hmje_ax_on_main(^{
            if (!g_view) return;
            if (!g_ax_elements) g_ax_elements = [NSMutableDictionary dictionary];
            BOOL topologyChanged = ![g_ax_current isEqualToSet:keys];
            NSMutableArray *ordered = [NSMutableArray arrayWithCapacity:count];
            NSMutableDictionary *next = [NSMutableDictionary dictionaryWithCapacity:count];
            for (NSDictionary *record in snapshot) {
                NSNumber *key = record[@"id"];
                u64 role = [record[@"role"] unsignedLongLongValue];
                u64 state = [record[@"state"] unsignedLongLongValue];
                u64 actions = (state & 1) ? 0 : [record[@"actions"] unsignedLongLongValue];
                CGRect frame = [record[@"frame"] CGRectValue];
                id element = g_ax_elements[key];
                BOOL editor = role == 3;
                if (element && [element isKindOfClass:[HmjeAccessibilityTextField class]] != editor) {
                    if ([element isKindOfClass:[UIView class]]) [element removeFromSuperview];
                    element = nil;
                }
                if (editor) {
                    HmjeAccessibilityTextField *field = element;
                    if (!field) {
                        field = [[HmjeAccessibilityTextField alloc] initWithFrame:frame];
                        field.borderStyle = UITextBorderStyleNone;
                        field.backgroundColor = UIColor.clearColor;
                        field.textColor = UIColor.clearColor;
                        field.tintColor = UIColor.clearColor;
                        field.userInteractionEnabled = NO;
                        field.isAccessibilityElement = YES;
                        [g_view addSubview:field];
                    }
                    field.semanticIdentifier = key.unsignedLongLongValue;
                    field.actions = actions;
                    field.semanticFocused = (state & 16) != 0;
                    field.frame = frame;
                    field.enabled = !(state & 1);
                    field.secureTextEntry = (state & 32) != 0;
                    field.accessibilityLabel = record[@"label"];
                    field.accessibilityValue = record[@"value"];
                    field.accessibilityIdentifier = [NSString stringWithFormat:@"hmje.%llu", field.semanticIdentifier];
                    field.accessibilityHint = (state & 4) ? @"여러 줄 텍스트 편집" : @"텍스트 편집";
                    element = field;
                } else {
                    HmjeAccessibilityElement *node = element;
                    if (!node) node = [[HmjeAccessibilityElement alloc] initWithAccessibilityContainer:g_view];
                    node.semanticIdentifier = key.unsignedLongLongValue;
                    node.actions = actions;
                    node.accessibilityLabel = record[@"label"];
                    node.accessibilityValue = record[@"value"];
                    node.accessibilityIdentifier = [NSString stringWithFormat:@"hmje.%llu", node.semanticIdentifier];
                    node.accessibilityFrameInContainerSpace = frame;
                    UIAccessibilityTraits traits = UIAccessibilityTraitNone;
                    if (role == 1) traits |= UIAccessibilityTraitStaticText;
                    if (role == 2 || role == 4 || role == 5) traits |= UIAccessibilityTraitButton;
                    if (role == 6) traits |= UIAccessibilityTraitAdjustable;
                    if (state & 1) traits |= UIAccessibilityTraitNotEnabled;
                    if (state & 2) traits |= UIAccessibilityTraitSelected;
                    node.accessibilityTraits = traits;
                    element = node;
                }
                next[key] = element;
                [ordered addObject:element];
            }
            for (NSNumber *key in g_ax_elements) {
                if (!next[key] && [g_ax_elements[key] isKindOfClass:[UIView class]])
                    [g_ax_elements[key] removeFromSuperview];
            }
            g_ax_elements = next;
            os_unfair_lock_lock(&g_ax_lock);
            g_ax_current = keys;
            os_unfair_lock_unlock(&g_ax_lock);
            g_view.isAccessibilityElement = NO;
            g_view.accessibilityElements = ordered;
            if (topologyChanged && UIAccessibilityIsVoiceOverRunning())
                UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil);
        });
        return (i64)count;
    }
}
