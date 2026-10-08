/* OS boundary: Security dictionaries, LAContext callbacks and ObjC lifetimes.
 * Migration sequencing and byte verification live in protected.hmje. */
#include "vault_protected.h"
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>
#include <string.h>

enum { VP_CANCEL = 8, VP_DENIED = 9, VP_PENDING = 10, VP_CLOSED = 11 };
#if __has_feature(objc_arc)
#define VP_RELEASE(x) ((void)0)
#define VP_AUTORELEASE(x) (x)
#else
#define VP_RELEASE(x) [(x) release]
#define VP_AUTORELEASE(x) [(x) autorelease]
#endif

/* No production environment-variable switches or mock authentication paths. */
#ifdef HMJE_VAULT_PROTECTED_TEST
extern LAContext *hmje_vp_test_context(void);
extern int64_t hmje_vp_test_support(void);
extern OSStatus hmje_vp_test_copy(CFDictionaryRef, CFTypeRef *);
extern OSStatus hmje_vp_test_add(CFDictionaryRef);
extern OSStatus hmje_vp_test_update(CFDictionaryRef, CFDictionaryRef);
extern OSStatus hmje_vp_test_delete(CFDictionaryRef);
extern int64_t hmje_vp_test_plain(const void *, int64_t, const void *, int64_t, void *, int64_t, uint64_t *);
#define VP_COPY(q,p) hmje_vp_test_copy((__bridge CFDictionaryRef)(q),p)
#define VP_ADD(q) hmje_vp_test_add((__bridge CFDictionaryRef)(q))
#define VP_UPDATE(q,a) hmje_vp_test_update((__bridge CFDictionaryRef)(q),(__bridge CFDictionaryRef)(a))
#define VP_DELETE(q) hmje_vp_test_delete((__bridge CFDictionaryRef)(q))
#define VP_PLAIN hmje_vp_test_plain
#else
#define VP_COPY(q,p) SecItemCopyMatching((__bridge CFDictionaryRef)(q),p)
#define VP_ADD(q) SecItemAdd((__bridge CFDictionaryRef)(q),NULL)
#define VP_UPDATE(q,a) SecItemUpdate((__bridge CFDictionaryRef)(q),(__bridge CFDictionaryRef)(a))
#define VP_DELETE(q) SecItemDelete((__bridge CFDictionaryRef)(q))
#define VP_PLAIN hmje_vault_get
#endif

@interface HMJEVaultPresenceRequest : NSObject {
@public
    LAContext *context;
    NSMutableDictionary *query;
    NSMutableData *input, *result;
    NSData *namespaceBytes, *keyBytes;
    uint64_t operation, policy;
    int64_t status;
    BOOL cancelled, closed, migrationReady;
    NSUInteger migrationStep;
}
@end
static void vp_wipe(NSMutableData *data) {
    if (data.length) hmje_vault_wipe(data.mutableBytes, (int64_t)data.length);
    [data setLength:0];
}
@implementation HMJEVaultPresenceRequest
- (void)dealloc {
    [context invalidate];
    vp_wipe(input); vp_wipe(result);
    VP_RELEASE(context); VP_RELEASE(query); VP_RELEASE(input); VP_RELEASE(result);
    VP_RELEASE(namespaceBytes); VP_RELEASE(keyBytes);
#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}
@end

static NSMutableDictionary *vp_requests(void) {
    static NSMutableDictionary *requests;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ requests = [[NSMutableDictionary alloc] init]; });
    return requests;
}
static HMJEVaultPresenceRequest *vp_lookup(uint64_t handle) {
    NSMutableDictionary *requests = vp_requests();
    @synchronized(requests) {
#if __has_feature(objc_arc)
        return requests[@(handle)];
#else
        return [[requests[@(handle)] retain] autorelease];
#endif
    }
}
static int64_t vp_support(void) {
#ifdef HMJE_VAULT_PROTECTED_TEST
    return hmje_vp_test_support();
#else
    /* A team signature alone does not authorize a Keychain access group. The
     * SecItem operation remains the final entitlement/provisioning check. */
    SecCodeRef code = NULL;
    CFDictionaryRef info = NULL;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess) return VAULT_UNSUPPORTED;
    OSStatus s = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info);
    CFRelease(code);
    BOOL supported = NO;
    if (s == errSecSuccess && info) {
        id team = (__bridge id)CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier);
        NSDictionary *entitlements = (__bridge id)CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict);
        id app = entitlements[@"com.apple.application-identifier"];
        id groups = entitlements[@"keychain-access-groups"];
        supported = [team isKindOfClass:[NSString class]] && [team length] &&
                    [app isKindOfClass:[NSString class]] && [app length] &&
                    [groups isKindOfClass:[NSArray class]] && [groups count];
    }
    if (info) CFRelease(info);
    return supported ? VAULT_OK : VAULT_UNSUPPORTED;
#endif
}
static int64_t vp_status(OSStatus s) {
    if (s == errSecSuccess) return VAULT_OK;
    if (s == errSecItemNotFound) return VAULT_MISSING;
    if (s == errSecUserCanceled) return VP_CANCEL;
    if (s == errSecAuthFailed) return VP_DENIED;
    if (s == errSecInteractionNotAllowed || s == errSecNotAvailable) return VAULT_LOCKED;
    if (s == errSecMissingEntitlement || s == errSecUnimplemented) return VAULT_UNSUPPORTED;
    if (s == errSecParam) return VAULT_INVALID;
    if (s == errSecDecode) return VAULT_INTEGRITY;
    return VAULT_IO;
}
static int64_t vp_la_status(NSError *error) {
    if (![error.domain isEqualToString:LAErrorDomain]) return VAULT_INTERNAL;
    switch (error.code) {
        case LAErrorUserCancel: case LAErrorAppCancel: case LAErrorSystemCancel: return VP_CANCEL;
        case LAErrorAuthenticationFailed: case LAErrorUserFallback: return VP_DENIED;
        case LAErrorBiometryLockout: case LAErrorNotInteractive: return VAULT_LOCKED;
        case LAErrorBiometryNotAvailable: case LAErrorBiometryNotEnrolled:
        case LAErrorPasscodeNotSet: return VAULT_UNSUPPORTED;
        default: return VAULT_INTERNAL;
    }
}
static NSMutableDictionary *vp_query(const void *ns, int64_t nl, const void *key, int64_t kl) {
#ifdef HMJE_VAULT_TEST_APP_ID
    NSString *app = @HMJE_VAULT_TEST_APP_ID;
#else
    NSString *app = [[NSBundle mainBundle] bundleIdentifier];
#endif
    if (!app.length || app.length > 255) return nil;
    char nh[VAULT_NAME_MAX * 2 + 1], kh[VAULT_NAME_MAX * 2 + 1];
    vault_hex(ns, (size_t)nl, nh); vault_hex(key, (size_t)kl, kh);
    return [NSMutableDictionary dictionaryWithDictionary:@{
        (id)kSecClass: (id)kSecClassGenericPassword,
        (id)kSecAttrService: [NSString stringWithFormat:@"net.semtle.hmje-vault.protected.v1.%@.%s",app,nh],
        (id)kSecAttrAccount: [NSString stringWithUTF8String:kh],
        (id)kSecAttrSynchronizable: @NO,
        (id)kSecUseDataProtectionKeychain: @YES
    }];
}
static int64_t vp_copy(NSMutableDictionary *query, void *out, int64_t capacity, uint64_t *length) {
    *length = 0;
    NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:query];
    q[(id)kSecReturnData] = @YES;
    q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef item = NULL;
    OSStatus s = VP_COPY(q, &item);
    if (s != errSecSuccess) { if (item) CFRelease(item); return vp_status(s); }
    if (!item || CFGetTypeID(item) != CFDataGetTypeID()) {
        if (item) CFRelease(item);
        return VAULT_INTEGRITY;
    }
    CFIndex n = CFDataGetLength(item);
    int64_t status = VAULT_OK;
    if (n < 0 || n > VAULT_VALUE_MAX) status = VAULT_INTEGRITY;
    else if (n > capacity) status = VAULT_INVALID;
    else {
        if (n) memcpy(out, CFDataGetBytePtr(item), (size_t)n);
        *length = (uint64_t)n;
    }
    CFRelease(item);
    return status;
}
static int64_t vp_write(HMJEVaultPresenceRequest *r, const void *bytes, int64_t length, BOOL addOnly) {
    CFErrorRef error = NULL;
    SecAccessControlRef access = SecAccessControlCreateWithFlags(kCFAllocatorDefault,
        kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
        r->policy == 1 ? kSecAccessControlUserPresence : kSecAccessControlBiometryCurrentSet, &error);
    if (error) CFRelease(error);
    if (!access) return VAULT_UNSUPPORTED;
    /* NSData borrows our wipeable bytes for this synchronous SecItem call. */
    NSData *data = [NSData dataWithBytesNoCopy:(void *)bytes length:(NSUInteger)length freeWhenDone:NO];
    NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:r->query];
    q[(id)kSecAttrAccessControl] = (__bridge id)access;
    q[(id)kSecAttrGeneric] = [NSData dataWithBytes:&r->policy length:sizeof(r->policy)];
    q[(id)kSecValueData] = data;
    OSStatus s = VP_ADD(q);
    CFRelease(access);
    if (s == errSecDuplicateItem && !addOnly) {
        /* Never silently retain a different ACL while reporting a new policy. */
        NSMutableDictionary *attrs = [NSMutableDictionary dictionaryWithDictionary:r->query];
        attrs[(id)kSecReturnAttributes] = @YES;
        CFTypeRef found = NULL;
        s = VP_COPY(attrs,&found);
        if (s == errSecSuccess) {
            NSDictionary *item = (__bridge id)found;
            NSData *stored = [item isKindOfClass:[NSDictionary class]] ? item[(id)kSecAttrGeneric] : nil;
            BOOL same = [stored isEqual:q[(id)kSecAttrGeneric]] && item[(id)kSecAttrAccessControl];
            if (!same) { if (found) CFRelease(found); return VAULT_INVALID; }
            s = VP_UPDATE(r->query,@{(id)kSecValueData:data});
        }
        if (found) CFRelease(found);
    }
    return vp_status(s);
}
static int64_t vp_check_policy(HMJEVaultPresenceRequest *r) {
    NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:r->query];
    q[(id)kSecReturnAttributes] = @YES;
    q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef found = NULL;
    OSStatus s = VP_COPY(q,&found);
    int64_t result = vp_status(s);
    if (s == errSecSuccess) {
        NSDictionary *item = (__bridge id)found;
        NSData *expected = [NSData dataWithBytes:&r->policy length:sizeof(r->policy)];
        if (![item isKindOfClass:[NSDictionary class]] ||
            ![item[(id)kSecAttrGeneric] isEqual:expected] || !item[(id)kSecAttrAccessControl]) result = VAULT_INVALID;
    }
    if (found) CFRelease(found);
    return result;
}
static void vp_complete(HMJEVaultPresenceRequest *r, BOOL accepted, NSError *error) {
    @autoreleasepool {
        @synchronized(r) {
            if (r->closed || r->cancelled) return;
            if (!accepted) {
                r->status = vp_la_status(error);
            } else {
                r->context.interactionNotAllowed = YES;
                if (r->operation == 4) {
                    r->migrationReady = YES;
                    r->status = VAULT_OK;
                    return; /* HMJE runs copy/add/readback and then invalidates. */
                }
                if (r->operation == 1) r->status = vp_write(r,r->input.bytes,(int64_t)r->input.length,NO);
                else if ((r->status = vp_check_policy(r)) != VAULT_OK) {
                    /* Missing, incompatible policy, or malformed metadata: no secret access. */
                } else if (r->operation == 2) {
                    r->result = [[NSMutableData alloc] initWithLength:VAULT_VALUE_MAX];
                    uint64_t n = 0;
                    r->status = vp_copy(r->query,r->result.mutableBytes,VAULT_VALUE_MAX,&n);
                    if (r->status == VAULT_OK) [r->result setLength:(NSUInteger)n];
                    else vp_wipe(r->result);
                } else r->status = vp_status(VP_DELETE(r->query));
            }
            vp_wipe(r->input);
            [r->context invalidate];
        }
    }
}
uint64_t hmje_vault_protected_start(uint64_t operation, const void *ns, int64_t nl,
        const void *key, int64_t kl, uint64_t policy, const void *prompt, int64_t pl,
        const void *value, int64_t size, uint64_t *status) {
    if (!status) return 0;
    *status = VAULT_INVALID;
    if (!vault_names(ns,nl,key,kl) || operation < 1 || operation > 4 || policy < 1 || policy > 2 ||
        pl <= 0 || pl > 4096 || !hmje_vault_utf8_valid(prompt,pl,0) || size < 0 ||
        size > VAULT_VALUE_MAX || (size && !value)) return 0;
    @autoreleasepool {
        *status = (uint64_t)vp_support();
        if (*status != VAULT_OK) return 0;
        HMJEVaultPresenceRequest *r = VP_AUTORELEASE([[HMJEVaultPresenceRequest alloc] init]);
        r->query = [vp_query(ns,nl,key,kl) mutableCopy];
        if (!r->query) { *status = VAULT_UNSUPPORTED; return 0; }
#ifdef HMJE_VAULT_PROTECTED_TEST
        r->context = hmje_vp_test_context();
#if !__has_feature(objc_arc)
        [r->context retain];
#endif
#else
        r->context = [[LAContext alloc] init];
#endif
        r->context.touchIDAuthenticationAllowableReuseDuration = 0;
        NSString *reason = VP_AUTORELEASE([[NSString alloc] initWithBytes:prompt length:(NSUInteger)pl encoding:NSUTF8StringEncoding]);
        r->context.localizedReason = reason;
        r->query[(id)kSecUseAuthenticationContext] = r->context;
        r->operation = operation; r->policy = policy; r->status = VP_PENDING;
        r->input = [[NSMutableData alloc] initWithBytes:value length:(NSUInteger)size];
        r->namespaceBytes = [[NSData alloc] initWithBytes:ns length:(NSUInteger)nl];
        r->keyBytes = [[NSData alloc] initWithBytes:key length:(NSUInteger)kl];
        LAPolicy p = policy == 1 ? LAPolicyDeviceOwnerAuthentication : LAPolicyDeviceOwnerAuthenticationWithBiometrics;
        NSError *error = nil;
        if (![r->context canEvaluatePolicy:p error:&error]) { *status = (uint64_t)vp_la_status(error); return 0; }
        static uint64_t nextHandle;
        uint64_t handle;
        NSMutableDictionary *requests = vp_requests();
        @synchronized(requests) {
            if (nextHandle == UINT64_MAX) { *status = VAULT_INTERNAL; return 0; }
            handle = ++nextHandle;
            requests[@(handle)] = r;
        }
        *status = VP_PENDING;
        [r->context evaluatePolicy:p localizedReason:reason reply:^(BOOL ok,NSError *e) {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{ vp_complete(r,ok,e); });
        }];
        return handle;
    }
}
int64_t hmje_vault_protected_exists(const void *ns,int64_t nl,const void *key,int64_t kl) {
    if (!vault_names(ns,nl,key,kl)) return VAULT_INVALID;
    @autoreleasepool {
        int64_t support = vp_support();
        if (support != VAULT_OK) return support;
        NSMutableDictionary *q = vp_query(ns,nl,key,kl);
        if (!q) return VAULT_UNSUPPORTED;
        LAContext *context = [[LAContext alloc] init];
        context.interactionNotAllowed = YES;
        q[(id)kSecUseAuthenticationContext] = context;
        q[(id)kSecReturnAttributes] = @YES;
        q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
        CFTypeRef found = NULL;
        OSStatus s = VP_COPY(q,&found);
        if (found) CFRelease(found);
        [context invalidate]; VP_RELEASE(context);
        return vp_status(s);
    }
}
int64_t hmje_vault_protected_poll(uint64_t handle,void *out,int64_t capacity,uint64_t *length) {
    if (length) *length = 0;
    if (!length || capacity < 0 || (capacity && !out)) return VAULT_INVALID;
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return VP_CLOSED;
        @synchronized(r) {
            if (r->closed) return VP_CLOSED;
            if (r->status != VAULT_OK) return r->status;
            if (r->result.length > (uint64_t)capacity) return VAULT_INVALID;
            if (r->result.length) memcpy(out,r->result.bytes,r->result.length);
            *length = r->result.length;
            return VAULT_OK;
        }
    }
}
int64_t hmje_vault_protected_cancel(uint64_t handle) {
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return VP_CLOSED;
        @synchronized(r) {
            if (r->closed) return VP_CLOSED;
            r->cancelled = YES; r->migrationReady = NO; r->status = VP_CANCEL;
            [r->context invalidate]; vp_wipe(r->input); vp_wipe(r->result);
            return VP_CANCEL;
        }
    }
}
int64_t hmje_vault_protected_close(uint64_t handle) {
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return VP_CLOSED;
        @synchronized(r) {
            r->closed = YES; r->migrationReady = NO;
            [r->context invalidate]; vp_wipe(r->input); vp_wipe(r->result);
        }
        NSMutableDictionary *requests = vp_requests();
        @synchronized(requests) { [requests removeObjectForKey:@(handle)]; }
        return VP_CLOSED;
    }
}
uint64_t hmje_vault_protected_migration_ready(uint64_t handle) {
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return 0;
        @synchronized(r) {
            /* Claim once: simultaneous pollers must not run migration twice. */
            if (!r->migrationReady || r->cancelled || r->closed) return 0;
            r->migrationReady = NO; r->status = VP_PENDING;
            return 1;
        }
    }
}
int64_t hmje_vault_protected_transfer(uint64_t handle,uint64_t step,void *data,int64_t length,uint64_t *written) {
    if (written) *written = 0;
    if (!written || length < 0 || length > VAULT_VALUE_MAX || (length && !data)) return VAULT_INVALID;
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return VP_CLOSED;
        @synchronized(r) {
            if (r->closed) return VP_CLOSED;
            if (r->cancelled) return VP_CANCEL;
            if (r->operation != 4 || r->status != VP_PENDING || r->migrationStep != step || step > 2) return VAULT_INVALID;
            int64_t s;
            if (step == 0) s = VP_PLAIN(r->namespaceBytes.bytes,(int64_t)r->namespaceBytes.length,
                r->keyBytes.bytes,(int64_t)r->keyBytes.length,data,length,written);
            else if (step == 1) s = vp_write(r,data,length,YES);
            else s = vp_copy(r->query,data,length,written);
            r->migrationStep++;
            return s;
        }
    }
}
int64_t hmje_vault_protected_finish(uint64_t handle,uint64_t status) {
    @autoreleasepool {
        HMJEVaultPresenceRequest *r = vp_lookup(handle);
        if (!r) return VP_CLOSED;
        @synchronized(r) {
            if (r->closed) return VP_CLOSED;
            if (r->cancelled) return VP_CANCEL;
            if (r->operation != 4 || r->status != VP_PENDING) return VAULT_INVALID;
            r->status = (int64_t)status;
            [r->context invalidate];
            return r->status;
        }
    }
}

int64_t hmje_vault_protected_availability(uint64_t *reason) {
    if (!reason) return VAULT_INVALID;
    *reason = 7;
    @autoreleasepool {
        int64_t supported = vp_support();
        if (supported != VAULT_OK) { *reason = 6; return supported; }
        LAContext *c = [[LAContext alloc] init];
        c.interactionNotAllowed = YES;
        NSError *error = nil;
        BOOL available = [c canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:&error];
        [c invalidate]; VP_RELEASE(c);
        if (available) { *reason = 0; return VAULT_OK; }
        if (error.code == LAErrorPasscodeNotSet || error.code == LAErrorBiometryNotEnrolled) *reason = 2;
        else if (error.code == LAErrorBiometryNotAvailable) *reason = 1;
        else if (error.code == LAErrorBiometryLockout || error.code == LAErrorNotInteractive) *reason = 4;
        return vp_la_status(error);
    }
}
