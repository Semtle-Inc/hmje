#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <string.h>
#include "file_dialog.h"

int64_t hmje_ui_file_dialog_supported(void) { return 1; }

int64_t hmje_ui_choose_file(int64_t action, uint8_t *output,
                            uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (action != 1 && action != 2)) return -1;
    if (![NSThread isMainThread]) return -2;
    @autoreleasepool {
        NSSavePanel *panel;
        if (action == 1) {
            NSOpenPanel *open = [NSOpenPanel openPanel];
            open.canChooseFiles = YES;
            open.canChooseDirectories = NO;
            open.allowsMultipleSelection = NO;
            panel = open;
        } else {
            panel = [NSSavePanel savePanel];
        }
        NSModalResponse result = [panel runModal];
        if (result != NSModalResponseOK) return 0;
        NSString *path = panel.URL.path;
        if (!path || !path.isAbsolutePath) return -2;
        NSData *bytes = [path dataUsingEncoding:NSUTF8StringEncoding];
        if (!bytes || !bytes.length) return -2;
        if (bytes.length > capacity) return -3;
        memcpy(output, bytes.bytes, bytes.length);
        *written = (uint64_t)bytes.length;
        return 1;
    }
}

int64_t hmje_ui_choose_folder(const uint8_t *initial, uint64_t initial_length,
                              const uint8_t *prompt, uint64_t prompt_length,
                              uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (initial_length && !initial) ||
        (prompt_length && !prompt) || initial_length > 131072 || prompt_length > 131072) return -1;
    if (![NSThread isMainThread]) return -2;
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        if (initial_length) {
            NSString *path = [[NSString alloc] initWithBytes:initial length:(NSUInteger)initial_length encoding:NSUTF8StringEncoding];
            if (!path || !path.isAbsolutePath) return -1;
            panel.directoryURL = [NSURL fileURLWithPath:path isDirectory:YES];
        }
        if (prompt_length) {
            NSString *label = [[NSString alloc] initWithBytes:prompt length:(NSUInteger)prompt_length encoding:NSUTF8StringEncoding];
            if (!label) return -1;
            panel.prompt = label;
        }
        if ([panel runModal] != NSModalResponseOK) return 0;
        NSData *bytes = [panel.URL.path dataUsingEncoding:NSUTF8StringEncoding];
        if (!bytes || !bytes.length) return -2;
        if (bytes.length > capacity) return -3;
        memcpy(output, bytes.bytes, bytes.length);
        *written = (uint64_t)bytes.length;
        return 1;
    }
}
