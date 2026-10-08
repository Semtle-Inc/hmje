#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);

static NSMutableArray<NSString *> *clicked;
static pthread_mutex_t clicked_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clicked_changed = PTHREAD_COND_INITIALIZER;
static int click_waiter_active;
@interface HmjeNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end
@implementation HmjeNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
 didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    pthread_mutex_lock(&clicked_lock);
    [clicked addObject:response.notification.request.identifier ?: @""];
    pthread_cond_broadcast(&clicked_changed);
    pthread_mutex_unlock(&clicked_lock);
    completionHandler();
}
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center; (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}
@end
static HmjeNotificationDelegate *notification_delegate;
static void ensure_notification_delegate(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        clicked = [NSMutableArray new];
        notification_delegate = [HmjeNotificationDelegate new];
        [UNUserNotificationCenter currentNotificationCenter].delegate = notification_delegate;
    });
}
static NSString *notification_string(const uint8_t *bytes, uint64_t length) {
    if (length > 131072 || (length && !bytes)) return nil;
    if (!length) return @"";
    return [[NSString alloc] initWithBytes:bytes length:(NSUInteger)length encoding:NSUTF8StringEncoding];
}
void hmje_ui_notification_request_async(uint64_t token) {
    ensure_notification_delegate();
    [[UNUserNotificationCenter currentNotificationCenter]
        requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionBadge | UNAuthorizationOptionSound)
                     completionHandler:^(BOOL granted, NSError *error) {
        hmje_async_complete_u64(token, error ? EIO : 0, granted ? 1 : 0);
    }];
}
void hmje_ui_notification_post_async(const uint8_t *title, uint64_t title_length,
    const uint8_t *body, uint64_t body_length, const uint8_t *identifier,
    uint64_t identifier_length, uint64_t token) {
    NSString *title_text = notification_string(title, title_length);
    NSString *body_text = notification_string(body, body_length);
    NSString *id_text = notification_string(identifier, identifier_length);
    if (!title_text || !body_text || !id_text || !id_text.length) {
        hmje_async_complete_u64(token, EINVAL, 0); return;
    }
    ensure_notification_delegate();
    UNMutableNotificationContent *content = [UNMutableNotificationContent new];
    content.title = title_text;
    content.body = body_text;
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:id_text
        content:content trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request
        withCompletionHandler:^(NSError *error) {
            hmje_async_complete_u64(token, error ? EIO : 0, error ? 0 : 1);
        }];
}
int64_t hmje_ui_notification_click_next(uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!output || !capacity || !written) return -EINVAL;
    ensure_notification_delegate();
    pthread_mutex_lock(&clicked_lock);
    if (!clicked.count) { pthread_mutex_unlock(&clicked_lock); return 0; }
    NSData *bytes = [clicked[0] dataUsingEncoding:NSUTF8StringEncoding];
    if (bytes.length > capacity) { pthread_mutex_unlock(&clicked_lock); return -ERANGE; }
    memcpy(output, bytes.bytes, bytes.length);
    *written = bytes.length;
    [clicked removeObjectAtIndex:0];
    pthread_mutex_unlock(&clicked_lock);
    return 1;
}
static void *notification_click_wait_worker(void *pointer) {
    uint64_t token = *(uint64_t *)pointer;
    free(pointer);
    pthread_mutex_lock(&clicked_lock);
    while (!clicked.count) pthread_cond_wait(&clicked_changed, &clicked_lock);
    pthread_mutex_unlock(&clicked_lock);
    hmje_async_complete_u64(token, 0, 1);
    pthread_mutex_lock(&clicked_lock);
    click_waiter_active = 0;
    pthread_mutex_unlock(&clicked_lock);
    return NULL;
}
void hmje_ui_notification_click_wait_async(uint64_t token) {
    ensure_notification_delegate();
    pthread_mutex_lock(&clicked_lock);
    if (click_waiter_active) {
        pthread_mutex_unlock(&clicked_lock);
        hmje_async_complete_u64(token, EBUSY, 0); return;
    }
    click_waiter_active = 1;
    pthread_mutex_unlock(&clicked_lock);
    uint64_t *request = malloc(sizeof(*request));
    if (!request) {
        pthread_mutex_lock(&clicked_lock); click_waiter_active = 0; pthread_mutex_unlock(&clicked_lock);
        hmje_async_complete_u64(token, ENOMEM, 0); return;
    }
    *request = token;
    pthread_t worker;
    int error = pthread_create(&worker, NULL, notification_click_wait_worker, request);
    if (error) {
        free(request);
        pthread_mutex_lock(&clicked_lock); click_waiter_active = 0; pthread_mutex_unlock(&clicked_lock);
        hmje_async_complete_u64(token, error, 0);
    } else pthread_detach(worker);
}
int64_t hmje_ui_notification_badge(int64_t number) {
    if (number < 0 || number > 999999 || ![NSThread isMainThread] || !NSApp) return -EINVAL;
    NSApp.dockTile.badgeLabel = number ? [NSString stringWithFormat:@"%lld", (long long)number] : nil;
    [NSApp.dockTile display];
    return 0;
}
