#include "vault.h"
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>
#import <TargetConditionals.h>
#include <string.h>

static NSString *vault_app_id(void) {
#ifdef HMJE_VAULT_TEST_APP_ID
    return @HMJE_VAULT_TEST_APP_ID;
#else
    return [[NSBundle mainBundle] bundleIdentifier];
#endif
}
static int valid_app_id(NSString *app) {
    const char *s = [app UTF8String];
    if (!s || !*s || strlen(s) > 255)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '-' || *s == '_'))
            return 0;
    return 1;
}
static int64_t vault_status(OSStatus s) {
#if defined(HMJE_VAULT_TEST_KEYCHAIN) || defined(HMJE_VAULT_TEST_TRACE)
    if (s != errSecSuccess && s != errSecItemNotFound)
        fprintf(stderr, "synthetic OSStatus=%d\n", (int)s);
#endif
    if (s == errSecSuccess)
        return VAULT_OK;
    if (s == errSecItemNotFound)
        return VAULT_MISSING;
    if (s == errSecInteractionNotAllowed || s == errSecAuthFailed || s == errSecNotAvailable)
        return VAULT_LOCKED;
    if (s == errSecMissingEntitlement)
        return VAULT_UNSUPPORTED;
    if (s == errSecDecode)
        return VAULT_INTEGRITY;
    if (s == errSecParam)
        return VAULT_INVALID;
    return VAULT_IO;
}
static NSMutableDictionary *vault_query(const void *ns, int64_t nl, const void *key, int64_t kl) {
    NSString *app = vault_app_id();
    if (!valid_app_id(app))
        return nil;
    char namespace_hex[VAULT_NAME_MAX * 2 + 1], key_hex[VAULT_NAME_MAX * 2 + 1];
    vault_hex(ns, (size_t)nl, namespace_hex);
    vault_hex(key, (size_t)kl, key_hex);
    NSString *service =
        [NSString stringWithFormat:@"net.semtle.hmje-vault.v1.%@.%s", app, namespace_hex];
    LAContext *authentication = [[LAContext alloc] init];
#if !__has_feature(objc_arc)
    [authentication autorelease];
#endif
    authentication.interactionNotAllowed = YES;
    NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:@{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : service,
        (id)kSecAttrAccount : [NSString stringWithUTF8String:key_hex],
        (id)kSecAttrSynchronizable : @NO,
        (id)kSecUseAuthenticationContext : authentication
    }];
#if TARGET_OS_OSX && !defined(HMJE_VAULT_TEST_KEYCHAIN)
    /* Keep iOS-style protection semantics on signed, entitled macOS apps. */
    q[(id)kSecUseDataProtectionKeychain] = @YES;
#endif
#if TARGET_OS_OSX && defined(HMJE_VAULT_TEST_KEYCHAIN)
    /* Synthetic host tests target their own disposable keychain, never login. */
    extern SecKeychainRef hmje_vault_test_keychain;
    [q setObject:(id)hmje_vault_test_keychain forKey:(id)kSecUseKeychain];
    [q setObject:@[ (id)hmje_vault_test_keychain ] forKey:(id)kSecMatchSearchList];
#endif
    return q;
}
int64_t hmje_vault_get(const void *ns, int64_t nl, const void *key, int64_t kl, void *out,
                       int64_t capacity, uint64_t *length) {
    if (length)
        *length = 0;
    if (!length || !vault_names(ns, nl, key, kl) || capacity < 0 || (capacity && !out))
        return VAULT_INVALID;
    @autoreleasepool {
        NSMutableDictionary *q = vault_query(ns, nl, key, kl);
        if (!q)
            return VAULT_UNSUPPORTED;
        q[(id)kSecReturnData] = @YES;
        q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
        CFTypeRef item = NULL;
        OSStatus status = SecItemCopyMatching((CFDictionaryRef)q, &item);
        if (status != errSecSuccess) {
            if (item)
                CFRelease(item);
            return vault_status(status);
        }
        if (!item || CFGetTypeID(item) != CFDataGetTypeID()) {
            if (item)
                CFRelease(item);
            return VAULT_INTEGRITY;
        }
        CFIndex size = CFDataGetLength((CFDataRef)item);
        if (size < 0 || size > VAULT_VALUE_MAX) {
            CFRelease(item);
            return VAULT_INTEGRITY;
        }
        if (size > capacity) {
            CFRelease(item);
            return VAULT_INVALID;
        }
        if (size)
            memcpy(out, CFDataGetBytePtr((CFDataRef)item), (size_t)size);
        *length = (uint64_t)size;
        CFRelease(item);
        return VAULT_OK;
    }
}
int64_t hmje_vault_set(const void *ns, int64_t nl, const void *key, int64_t kl, const void *value,
                       int64_t size) {
    if (!vault_names(ns, nl, key, kl) || size < 0 || size > VAULT_VALUE_MAX || (size && !value))
        return VAULT_INVALID;
    @autoreleasepool {
        NSMutableDictionary *q = vault_query(ns, nl, key, kl);
        if (!q)
            return VAULT_UNSUPPORTED;
        NSData *data = [NSData dataWithBytes:value length:(NSUInteger)size];
        NSDictionary *attrs = @{
            (id)kSecValueData : data,
            (id)kSecAttrAccessible : (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        };
        OSStatus status = SecItemUpdate((CFDictionaryRef)q, (CFDictionaryRef)attrs);
        if (status == errSecItemNotFound) {
            NSMutableDictionary *add = [NSMutableDictionary dictionaryWithDictionary:q];
            [add addEntriesFromDictionary:attrs];
#if TARGET_OS_OSX && defined(HMJE_VAULT_TEST_KEYCHAIN)
            [add removeObjectForKey:(id)kSecMatchSearchList];
#endif
            status = SecItemAdd((CFDictionaryRef)add, NULL);
            /* Another process may have inserted between update and add. */
            if (status == errSecDuplicateItem)
                status = SecItemUpdate((CFDictionaryRef)q, (CFDictionaryRef)attrs);
        }
        return vault_status(status);
    }
}
int64_t hmje_vault_delete(const void *ns, int64_t nl, const void *key, int64_t kl) {
    if (!vault_names(ns, nl, key, kl))
        return VAULT_INVALID;
    @autoreleasepool {
        NSMutableDictionary *q = vault_query(ns, nl, key, kl);
        if (!q)
            return VAULT_UNSUPPORTED;
        return vault_status(SecItemDelete((CFDictionaryRef)q));
    }
}
int64_t hmje_vault_app_data_path(void *out, int64_t capacity, uint64_t *length) {
    if (length)
        *length = 0;
    if (!length || !out || capacity <= 0)
        return VAULT_INVALID;
    @autoreleasepool {
        NSString *app = vault_app_id();
        if (!valid_app_id(app))
            return VAULT_UNSUPPORTED;
        NSError *error = nil;
        NSURL *base = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
                                                             inDomain:NSUserDomainMask
                                                    appropriateForURL:nil
                                                               create:YES
                                                                error:&error];
        if (!base || error)
            return VAULT_IO;
#if TARGET_OS_OSX
        base = [base URLByAppendingPathComponent:app isDirectory:YES];
#endif
        base = [base URLByAppendingPathComponent:@"hmje-data" isDirectory:YES];
        if (![[NSFileManager defaultManager] createDirectoryAtURL:base
                                      withIntermediateDirectories:YES
                                                       attributes:@{
                                                           NSFilePosixPermissions : @0700
                                                       }
                                                            error:&error])
            return VAULT_IO;
        NSData *utf8 = [[base path] dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
        NSUInteger size = [utf8 length];
        if (!utf8 || size > VAULT_PATH_MAX || size > (uint64_t)capacity)
            return VAULT_INVALID;
        memcpy(out, [utf8 bytes], size);
        *length = size;
        return VAULT_OK;
    }
}
