// macOS signing policy shared by the product coordinator and IDE. Public
// certificate bytes only; private keys remain in codesign's keychain service.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>

static NSString *Run(NSArray<NSString *> *args, int *status) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:args[0]];
    task.arguments = [args subarrayWithRange:NSMakeRange(1, args.count - 1)];
    NSPipe *out = [NSPipe pipe]; task.standardOutput = out;
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) { *status = -1; return @""; }
    NSData *data = [out.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit]; *status = task.terminationStatus;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}
static NSDictionary *Plist(NSData *data) {
    id value = data ? [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL] : nil;
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}
static NSString *Hash(NSData *cert) {
    if (![cert isKindOfClass:NSData.class]) return @"";
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA1(cert.bytes, (CC_LONG)cert.length, digest);
#pragma clang diagnostic pop
    NSMutableString *s = [NSMutableString new];
    for (unsigned i = 0; i < sizeof digest; ++i) [s appendFormat:@"%02X", digest[i]];
    return s;
}
static BOOL WriteEntitlements(NSDictionary *ent, NSString *path) {
    NSData *xml = [NSPropertyListSerialization dataWithPropertyList:ent format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    return [xml writeToFile:path atomically:YES];
}
static NSArray *Identities(void) {
    int status; NSString *text = Run(@[@"/usr/bin/security", @"find-identity", @"-v", @"-p", @"codesigning"], &status);
    NSMutableArray *result = [NSMutableArray new];
    NSRegularExpression *rx = [NSRegularExpression regularExpressionWithPattern:@"[0-9]+\\) ([A-Fa-f0-9]{40}) \\\"([^\\\"]+)\\\"" options:0 error:NULL];
    for (NSTextCheckingResult *m in [rx matchesInString:text options:0 range:NSMakeRange(0, text.length)])
        [result addObject:@{ @"hash": [[text substringWithRange:[m rangeAtIndex:1]] uppercaseString], @"name": [text substringWithRange:[m rangeAtIndex:2]] }];
    return result;
}
static NSDictionary *Identity(NSString *selection, NSArray *identities) {
    NSMutableArray *matches = [NSMutableArray new];
    for (NSDictionary *i in identities)
        if ([i[@"hash"] caseInsensitiveCompare:selection] == NSOrderedSame || [i[@"name"] isEqual:selection]) [matches addObject:i];
    return matches.count == 1 ? matches[0] : nil;
}
static NSDictionary *Decode(NSString *path) {
    int status; NSString *xml = Run(@[@"/usr/bin/security", @"cms", @"-D", @"-i", path], &status);
    return status == 0 ? Plist([xml dataUsingEncoding:NSUTF8StringEncoding]) : nil;
}
// Profile strings support only Apple's terminal wildcard, never glob character classes.
static BOOL Matches(NSString *allow, NSString *request) {
    if (![allow isKindOfClass:NSString.class] || ![request isKindOfClass:NSString.class]) return NO;
    if ([allow hasSuffix:@"*"]) {
        // -hasPrefix: is NO for an empty prefix, so a bare "*" must be handled explicitly.
        NSString *prefix = [allow substringToIndex:allow.length - 1];
        return !prefix.length || [request hasPrefix:prefix];
    }
    return [allow isEqual:request];
}
static BOOL Allowed(id allow, id request) {
    if ([request isKindOfClass:NSString.class]) return Matches(allow, request);
    if ([request isKindOfClass:NSArray.class]) {
        // Apple grants list-valued entitlements such as associated-domains as a
        // single wildcard string ("*"); every requested item must match it.
        if ([allow isKindOfClass:NSString.class]) {
            for (id item in request) if (!Matches(allow, item)) return NO;
            return YES;
        }
        if (![allow isKindOfClass:NSArray.class]) return NO;
        for (id item in request) {
            BOOL found = NO; for (id grant in allow) if (Allowed(grant, item)) { found = YES; break; }
            if (!found) return NO;
        }
        return YES;
    }
    if ([request isKindOfClass:NSDictionary.class]) {
        if (![allow isKindOfClass:NSDictionary.class]) return NO;
        for (id key in request) if (!Allowed(allow[key], request[key])) return NO;
        return YES;
    }
    return allow && [allow isEqual:request];
}
static NSArray *Restricted(NSDictionary *ent) {
    NSMutableArray *keys = [NSMutableArray new];
    for (NSString *key in ent) {
        BOOL restricted = [@[@"keychain-access-groups", @"application-identifier", @"com.apple.application-identifier", @"com.apple.security.application-groups"] containsObject:key] || [key hasPrefix:@"com.apple.developer."];
        if (restricted) [keys addObject:key];
    }
    return [keys sortedArrayUsingSelector:@selector(compare:)];
}
static NSString *Validate(NSDictionary *p, NSString *bundle, NSDictionary *identity, NSDictionary *ent) {
    if (!p) return @"프로비저닝 프로필을 security cms -D로 해독하지 못했습니다";
    id platforms = p[@"Platform"];
    if (![platforms isKindOfClass:NSArray.class] || (![platforms containsObject:@"OSX"] && ![platforms containsObject:@"macOS"])) return @"프로필 Platform이 macOS(OSX)가 아닙니다";
    id expiry = p[@"ExpirationDate"];
    if (![expiry isKindOfClass:NSDate.class] || [expiry timeIntervalSinceNow] <= 0) return @"프로필 ExpirationDate가 만료되었거나 없습니다";
    NSDictionary *grants = p[@"Entitlements"];
    if (![grants isKindOfClass:NSDictionary.class]) return @"프로필 Entitlements 사전이 없습니다";
    NSString *app = grants[@"com.apple.application-identifier"] ?: grants[@"application-identifier"];
    NSArray *teams = p[@"TeamIdentifier"];
    if (![teams isKindOfClass:NSArray.class] || !teams.count || ![teams[0] isKindOfClass:NSString.class]) return @"프로필 TeamIdentifier가 없습니다";
    NSString *expected = [NSString stringWithFormat:@"%@.%@", teams[0], bundle];
    if (!Matches(app, expected)) return [NSString stringWithFormat:@"프로필 application-identifier 불일치: %@, 필요한 값 %@", app ?: @"없음", expected];
    if (!identity) return @"HMJE_MACOS_SIGNING_IDENTITY에 해당하는 유효한 서명 인증서가 없거나 이름이 중복됩니다 (SHA-1 식별자를 사용하세요)";
    BOOL certMatch = NO;
    id certs = p[@"DeveloperCertificates"];
    if ([certs isKindOfClass:NSArray.class]) for (NSData *cert in certs) if ([Hash(cert) isEqual:identity[@"hash"]]) certMatch = YES;
    if (!certMatch) return [NSString stringWithFormat:@"프로필 DeveloperCertificates에 서명 인증서 %@가 없습니다", identity[@"name"]];
    for (NSString *key in ent) {
        id value = ent[key];
        if ([@[@"application-identifier", @"com.apple.application-identifier"] containsObject:key] && ![value isEqual:expected]) return [NSString stringWithFormat:@"권한 %@ 불일치: 필요한 값 %@", key, expected];
        if ([key isEqual:@"com.apple.developer.team-identifier"] && ![value isEqual:teams[0]]) return @"권한 com.apple.developer.team-identifier가 프로필 TeamIdentifier와 다릅니다";
        // macOS sandbox/hardened-runtime/TCC entitlements do not require a profile grant.
        if (!grants[key] && [key hasPrefix:@"com.apple.security."] && ![key isEqual:@"com.apple.security.application-groups"]) continue;
        if (!Allowed(grants[key], value)) return [NSString stringWithFormat:@"프로필이 요청 권한 %@의 값 %@를 허용하지 않습니다", key, value];
    }
    return @"";
}
static NSArray *Profiles(void) {
    NSMutableArray *result = [NSMutableArray new];
    NSString *home = NSProcessInfo.processInfo.environment[@"HOME"] ?: NSHomeDirectory();
    for (NSString *dir in @[@"Library/Developer/Xcode/UserData/Provisioning Profiles", @"Library/MobileDevice/Provisioning Profiles"]) {
        NSString *root = [home stringByAppendingPathComponent:dir];
        for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:NULL] sortedArrayUsingSelector:@selector(compare:)]) {
            if (![@[@"provisionprofile", @"mobileprovision"] containsObject:name.pathExtension]) continue;
            NSString *path = [root stringByAppendingPathComponent:name]; NSDictionary *data = Decode(path);
            if (data) [result addObject:@{ @"path":path, @"data":data }];
        }
    }
    return result;
}
static BOOL ContainsBinaryPath(NSString *line, NSString *path) {
    line = line.precomposedStringWithCanonicalMapping;
    path = path.precomposedStringWithCanonicalMapping;
    NSString *pattern = [[NSRegularExpression escapedPatternForString:path] stringByAppendingString:@"(?=$|[\\s'\"),:])"];
    NSRegularExpression *rx = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL];
    return [rx firstMatchInString:line options:0 range:NSMakeRange(0, line.length)] != nil;
}
static NSString *AmfiMessage(NSString *log, NSString *path) {
    NSString *resolved = [path stringByResolvingSymlinksInPath];
    for (NSString *line in [log componentsSeparatedByString:@"\n"]) {
        NSString *lower = line.lowercaseString;
        BOOL rejection = [lower containsString:@"restricted entitlement"] || [lower containsString:@"applemobilefileintegrityerror"] || [lower containsString:@"invalid signature"] || [lower containsString:@"signature is not valid"] || [lower containsString:@"signature validation failed"] || [lower containsString:@"rejected"];
        if ((ContainsBinaryPath(line, resolved) || ContainsBinaryPath(line, path)) && rejection)
            return @"macOS가 서명/권한 문제로 앱 실행을 거부했습니다. 프로젝트의 macOS 서명 설정에서 개발 인증서와 일치하는 프로비저닝 프로필을 선택하세요.";
    }
    return @"";
}
static void Line(NSString *text) { puts(text.UTF8String); }
static int Fail(NSString *text) { fprintf(stderr, "macOS 서명 오류: %s\n", text.UTF8String); return 2; }
static NSString *Field(NSString *text) {
    return [[text stringByReplacingOccurrencesOfString:@"\t" withString:@" "] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc < 2) return Fail(@"사용법: prepare|resolve|catalog|amfi");
    NSString *mode = @(argv[1]);
    if ([mode isEqual:@"amfi"] && argc == 3) {
        int status; NSString *log = Run(@[@"/usr/bin/log", @"show", @"--last", @"2m", @"--style", @"compact", @"--predicate", @"process == \"amfid\""], &status);
        NSString *message = AmfiMessage(log, @(argv[2]));
        if (message.length) { Line(message); return 0; }
        return 1;
    }
    if (![@[@"prepare", @"resolve", @"catalog"] containsObject:mode]) return Fail(@"알 수 없는 서명 명령");
    BOOL prepare = [mode isEqual:@"prepare"];
    if ((prepare && argc != 5) || ([mode isEqual:@"resolve"] && argc != 6) || ([mode isEqual:@"catalog"] && argc != 4)) return Fail(@"잘못된 서명 도구 인자");
    NSString *bundle = @(argv[2]), *entPath = @(argv[3]);
    if (prepare) {
        NSDictionary *info = Plist([NSData dataWithContentsOfFile:[bundle stringByAppendingPathComponent:@"Contents/Info.plist"]]);
        bundle = info[@"CFBundleIdentifier"];
        if (![bundle isKindOfClass:NSString.class] || !bundle.length) return Fail(@"Info.plist CFBundleIdentifier가 없습니다");
    }
    NSDictionary *ent = entPath.length ? Plist([NSData dataWithContentsOfFile:entPath]) : @{};
    if (!ent) return Fail([@"권한 plist를 읽지 못했습니다: " stringByAppendingString:entPath]);
    NSArray *restricted = Restricted(ent);
    if (prepare) [NSFileManager.defaultManager removeItemAtPath:@(argv[4]) error:NULL];
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    NSString *selection = prepare ? (env[@"HMJE_MACOS_SIGNING_IDENTITY"] ?: @"-") : (argc >= 5 ? @(argv[4]) : @"auto");
    if (!selection.length) selection = @"-";
    NSString *profile = prepare ? (env[@"HMJE_MACOS_PROVISIONING_PROFILE"] ?: @"") : (argc >= 6 ? @(argv[5]) : @"");
    if (!prepare && [mode isEqual:@"resolve"] && [selection isEqual:@"auto"] && !restricted.count) { Line(@"-\t"); return 0; }
    if (!prepare && [mode isEqual:@"resolve"] && [selection isEqual:@"-"]) { Line(@"-\t"); return 0; }
    if (prepare && [selection isEqual:@"-"] && !profile.length) {
        if (restricted.count) {
            NSString *message = [NSString stringWithFormat:@"제한된 권한 (%@)을 임시(ad-hoc) 서명했습니다. macOS가 실행 시 앱을 종료합니다. HMJE_MACOS_SIGNING_IDENTITY에 개발 인증서, HMJE_MACOS_PROVISIONING_PROFILE에 일치하는 프로필을 지정하세요.", [restricted componentsJoinedByString:@", "]];
            fprintf(stderr, "경고: %s\n", message.UTF8String);
            NSData *json = [NSJSONSerialization dataWithJSONObject:@{ @"code":@"restricted-entitlements-adhoc", @"launchAllowed":@NO, @"restrictedKeys":restricted, @"message":message } options:NSJSONWritingSortedKeys error:NULL];
            fprintf(stderr, "HMJE_MACOS_SIGNING_DIAGNOSTIC\t%s\n", [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
        }
        [NSFileManager.defaultManager removeItemAtPath:[@(argv[2]) stringByAppendingPathComponent:@"Contents/embedded.provisionprofile"] error:NULL];
        if (entPath.length && !WriteEntitlements(ent, @(argv[4]))) return Fail(@"서명 권한 파일을 쓰지 못했습니다");
        return 0;
    }
    NSArray *identities = Identities();
    if (prepare || ([mode isEqual:@"resolve"] && ![selection isEqual:@"auto"])) {
        NSDictionary *identity = Identity(selection, identities);
        if (!identity) return Fail(@"유효한 서명 인증서를 찾지 못했거나 이름이 중복됩니다. SHA-1 식별자를 사용하세요");
        if (!profile.length) {
            if (restricted.count) return Fail(@"제한된 권한에 HMJE_MACOS_PROVISIONING_PROFILE이 필요합니다");
            if (!prepare) { Line([selection stringByAppendingString:@"\t"]); return 0; }
            [NSFileManager.defaultManager removeItemAtPath:[@(argv[2]) stringByAppendingPathComponent:@"Contents/embedded.provisionprofile"] error:NULL];
            if (entPath.length && !WriteEntitlements(ent, @(argv[4]))) return Fail(@"서명 권한 파일을 쓰지 못했습니다");
            return 0;
        }
        NSDictionary *p = Decode(profile); NSString *error = Validate(p, bundle, identity, ent);
        if (error.length) return Fail([NSString stringWithFormat:@"%@: %@", profile, error]);
        if (!prepare) { Line([NSString stringWithFormat:@"%@\t%@", identity[@"hash"], profile]); return 0; }
        NSMutableDictionary *final = [ent mutableCopy];
        NSDictionary *grants = p[@"Entitlements"];
        NSString *appKey = grants[@"com.apple.application-identifier"] ? @"com.apple.application-identifier" : @"application-identifier";
        final[appKey] = [NSString stringWithFormat:@"%@.%@", p[@"TeamIdentifier"][0], bundle];
        if (grants[@"com.apple.developer.team-identifier"]) final[@"com.apple.developer.team-identifier"] = grants[@"com.apple.developer.team-identifier"];
        NSData *xml = [NSPropertyListSerialization dataWithPropertyList:final format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (![xml writeToFile:@(argv[4]) atomically:YES]) return Fail(@"서명 권한 파일을 쓰지 못했습니다");
        NSString *destination = [@(argv[2]) stringByAppendingPathComponent:@"Contents/embedded.provisionprofile"];
        [NSFileManager.defaultManager removeItemAtPath:destination error:NULL];
        if (![NSFileManager.defaultManager copyItemAtPath:profile toPath:destination error:NULL]) return Fail(@"Contents/embedded.provisionprofile 복사 실패");
        return 0;
    }
    NSArray *profiles = Profiles();
    for (NSDictionary *identity in identities) {
        if ([mode isEqual:@"resolve"] && ![identity[@"name"] hasPrefix:@"Apple Development:"]) continue;
        if ([mode isEqual:@"catalog"]) Line([NSString stringWithFormat:@"%@\t%@\t\t프로필 없음", identity[@"hash"], Field(identity[@"name"])]);
        for (NSDictionary *p in profiles) {
            if (Validate(p[@"data"], bundle, identity, ent).length) continue;
            if ([mode isEqual:@"resolve"]) { Line([NSString stringWithFormat:@"%@\t%@", identity[@"hash"], p[@"path"]]); return 0; }
            Line([NSString stringWithFormat:@"%@\t%@\t%@\t%@", identity[@"hash"], Field(identity[@"name"]), p[@"path"], Field(p[@"data"][@"Name"] ?: @"프로필")]);
        }
    }
    if ([mode isEqual:@"catalog"]) return 0;
    return Fail(@"제한된 권한에 맞는 Apple Development 인증서와 macOS 프로비저닝 프로필이 없습니다. 프로젝트의 macOS 서명 설정을 확인하세요");
}}
