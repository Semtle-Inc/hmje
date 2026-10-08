#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
#include "watch_common.h"

typedef struct {
    WatchBase base;
    FSEventStreamRef stream;
    dispatch_queue_t queue;
    CFMutableSetRef known;
} AppleWatch;

static void watch_callback(ConstFSEventStreamRef stream, void *context, size_t count,
                           void *paths, const FSEventStreamEventFlags flags[],
                           const FSEventStreamEventId ids[]) {
    (void)stream; (void)ids;
    AppleWatch *watch = context;
    char **names = paths;
    for (size_t i = 0; i < count; ++i) {
        uint64_t kind = 2; /* modified */
        if (flags[i] & kFSEventStreamEventFlagItemRemoved) kind = 3;
        else if (flags[i] & kFSEventStreamEventFlagItemRenamed) kind = 4;
        else if (flags[i] & kFSEventStreamEventFlagItemCreated) kind = 1;
        CFStringRef key = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
        if (key) {
            if (kind == 1 && CFSetContainsValue(watch->known, key)) kind = 2;
            if (kind == 3) CFSetRemoveValue(watch->known, key);
            else CFSetAddValue(watch->known, key);
            CFRelease(key);
        }
        watch_push(&watch->base, names[i], kind);
    }
}

int64_t hmje_ui_watch_start(const uint8_t *root, uint64_t root_length,
                             const uint8_t *excludes, uint64_t exclude_length,
                             uint64_t delay_ms) {
    if (!root || !root_length || root[0] != '/' ||
        (exclude_length && !excludes) || delay_ms > 60000) return -EINVAL;
    char *root_text = watch_span(root, root_length);
    char *exclude_text = exclude_length ? watch_span(excludes, exclude_length) : NULL;
    if (!root_text || (exclude_length && !exclude_text)) {
        free(root_text); free(exclude_text); return -EINVAL;
    }
    BOOL is_directory = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:root_text]
                                                isDirectory:&is_directory] || !is_directory) {
        free(root_text); free(exclude_text); return -ENOENT;
    }
    AppleWatch *watch = calloc(1, sizeof(*watch));
    if (!watch) { free(root_text); free(exclude_text); return -ENOMEM; }
    watch_base_init(&watch->base, root_text, exclude_text, delay_ms);
    watch->known = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
    if (!watch->known) { watch_base_destroy(&watch->base); free(watch); return -ENOMEM; }
    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:root_text];
        CFArrayRef paths = (__bridge CFArrayRef)@[path];
        FSEventStreamContext context = {0, watch, NULL, NULL, NULL};
        watch->stream = FSEventStreamCreate(NULL, watch_callback, &context, paths,
            kFSEventStreamEventIdSinceNow, 0.05,
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        if (!watch->stream) { CFRelease(watch->known); watch_base_destroy(&watch->base); free(watch); return -EIO; }
        watch->queue = dispatch_queue_create("hmje.ui.watch", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(watch->stream, watch->queue);
        if (!FSEventStreamStart(watch->stream)) {
            FSEventStreamInvalidate(watch->stream); FSEventStreamRelease(watch->stream);
            CFRelease(watch->known); watch_base_destroy(&watch->base); free(watch); return -EIO;
        }
    }
    return (int64_t)(uintptr_t)watch;
}

int64_t hmje_ui_watch_stop(int64_t handle) {
    if (handle <= 0) return -EINVAL;
    AppleWatch *watch = (AppleWatch *)(uintptr_t)handle;
    FSEventStreamStop(watch->stream);
    FSEventStreamInvalidate(watch->stream);
    FSEventStreamRelease(watch->stream);
    dispatch_sync(watch->queue, ^{});
    CFRelease(watch->known);
    watch_base_destroy(&watch->base);
    free(watch);
    return 0;
}
int64_t hmje_ui_watch_next(int64_t handle, uint8_t *output, uint64_t capacity, uint64_t *written) {
    return handle > 0 ? watch_next(&((AppleWatch *)(uintptr_t)handle)->base, output, capacity, written) : -EINVAL;
}
void hmje_ui_watch_wait_async(int64_t handle, uint64_t token) {
    if (handle <= 0) { hmje_async_complete_u64(token, EINVAL, 0); return; }
    watch_wait_async(&((AppleWatch *)(uintptr_t)handle)->base, token);
}
