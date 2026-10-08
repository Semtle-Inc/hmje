#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include "external_url.h"

int64_t hmje_ui_open_external_url(const uint8_t *bytes, uint64_t length) {
    @autoreleasepool {
        if (bytes == NULL || length == 0 || length > 4096) return 0;
        NSString *text = [[NSString alloc] initWithBytes:bytes length:(NSUInteger)length encoding:NSUTF8StringEncoding];
        if (text == nil) return 0;
        NSURL *url = [NSURL URLWithString:text];
        NSString *scheme = url.scheme.lowercaseString;
        if (url == nil || url.host.length == 0 || !([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"])) return 0;
#if TARGET_OS_IPHONE
        dispatch_async(dispatch_get_main_queue(), ^{
            [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
        });
#else
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSWorkspace.sharedWorkspace openURL:url];
        });
#endif
        return 1;
    }
}
