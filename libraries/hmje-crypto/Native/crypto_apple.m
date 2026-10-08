#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "crypto.h"
#include <string.h>
static SecKeyRef import_key(const void *bytes,size_t len,int private_key){
    CFDataRef data=CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,bytes,len,kCFAllocatorNull);
    if(!data)return NULL;
    NSDictionary *attrs=@{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
                         (__bridge id)kSecAttrKeyClass:private_key?(__bridge id)kSecAttrKeyClassPrivate:(__bridge id)kSecAttrKeyClassPublic,
                         (__bridge id)kSecAttrKeySizeInBits:@256};
    CFErrorRef error=NULL;SecKeyRef key=SecKeyCreateWithData(data,(__bridge CFDictionaryRef)attrs,&error);
    if(error)CFRelease(error);CFRelease(data);return key;
}
/* SecKey accepts mismatched public||scalar pairs. Prove their relationship with
 * a local OS ECDSA sign/verify over a fixed public digest; signature never leaves
 * this function or authenticates protocol data. No curve arithmetic is ours. */
static int pair_valid(SecKeyRef private_key,SecKeyRef public_key){
    unsigned char digest[32]={0x48,0x4d,0x4a,0x45,0x2d,0x50,0x32,0x35,0x36};
    CFDataRef d=CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,digest,32,kCFAllocatorNull);
    if(!d)return 0;
    int valid=0;
    /* This is only a local relationship proof, never a protocol signature.
     * Some valid software-key signatures are rejected by Security on macOS26
     * although an independent provider verifies the same public input. Retry
     * verification failures with a fresh OS signature, at most3 times. We must
     * still obtain a real successful verification; mismatched pairs fail closed.
     * Signing/provider errors are not retried or interpreted as validity. */
    for(int attempt=0;attempt<3 && !valid;attempt++){
        CFErrorRef error=NULL;
        CFDataRef signature=SecKeyCreateSignature(private_key,kSecKeyAlgorithmECDSASignatureDigestX962SHA256,d,&error);
        if(error || !signature){if(error)CFRelease(error);if(signature)CFRelease(signature);break;}
        valid=SecKeyVerifySignature(public_key,kSecKeyAlgorithmECDSASignatureDigestX962SHA256,d,signature,&error);
        int retry=!error || (CFEqual(CFErrorGetDomain(error),kCFErrorDomainOSStatus) && CFErrorGetCode(error)==errSecVerifyFailed);
        if(error)CFRelease(error);CFRelease(signature);
        if(!valid && !retry)break;
    }
    CFRelease(d);return valid;
}
static int public_valid(SecKeyRef key){
    /* Security validates the public point at EC key import. Key-exchange
     * capability is queried on the private key, where the operation belongs. */
    return key != NULL;
}
int64_t hmje_crypto_p256_public(const void *p,int64_t n){@autoreleasepool{
    if(!crypto_public_format(p,n))return CRYPTO_INVALID;
    SecKeyRef key=import_key(p,65,0);int valid=public_valid(key);if(key)CFRelease(key);
    return valid?CRYPTO_OK:CRYPTO_KEY_INVALID;
}}
int64_t hmje_crypto_p256_pair(const void *p,int64_t n){@autoreleasepool{
    if(!crypto_private_format(p,n))return CRYPTO_INVALID;
    SecKeyRef private_key=import_key(p,97,1),public_key=import_key(p,65,0);
    int valid=private_key && public_valid(public_key) && pair_valid(private_key,public_key);
    if(private_key)CFRelease(private_key);if(public_key)CFRelease(public_key);
    return valid?CRYPTO_OK:CRYPTO_KEY_INVALID;
}}
int64_t hmje_crypto_p256_generate(void *out,int64_t cap){@autoreleasepool{
    int result=crypto_generate_args(out,cap);if(result)return result;
    NSDictionary *attrs=@{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeECSECPrimeRandom,(__bridge id)kSecAttrKeySizeInBits:@256,(__bridge id)kSecPrivateKeyAttrs:@{(__bridge id)kSecAttrIsPermanent:@NO}};
    CFErrorRef error=NULL;SecKeyRef key=SecKeyCreateRandomKey((__bridge CFDictionaryRef)attrs,&error);
    if(error){CFRelease(error);error=NULL;}if(!key)return CRYPTO_INTERNAL;
    CFDataRef raw=SecKeyCopyExternalRepresentation(key,&error);if(error)CFRelease(error);
    result=CRYPTO_INTERNAL;
    if(raw && CFDataGetLength(raw)==97 && crypto_private_format(CFDataGetBytePtr(raw),97)){
        result=(int)hmje_crypto_p256_pair(CFDataGetBytePtr(raw),97);
        if(!result)memcpy(out,CFDataGetBytePtr(raw),97);
    }
    if(raw)CFRelease(raw);CFRelease(key);return result;
}}
int64_t hmje_crypto_p256_agree(const void *p,int64_t n,const void *peer,int64_t pn,void *out,int64_t cap){@autoreleasepool{
    int result=crypto_agree_args(p,n,peer,pn,out,cap);if(result)return result;
    SecKeyRef key=import_key(p,97,1),own=import_key(p,65,0),remote=import_key(peer,65,0);
    result=CRYPTO_KEY_INVALID;
    if(key && SecKeyIsAlgorithmSupported(key,kSecKeyOperationTypeKeyExchange,kSecKeyAlgorithmECDHKeyExchangeStandard) && public_valid(own) && public_valid(remote) && pair_valid(key,own)){
        CFErrorRef error=NULL;
        CFDataRef secret=SecKeyCopyKeyExchangeResult(key,kSecKeyAlgorithmECDHKeyExchangeStandard,remote,(__bridge CFDictionaryRef)@{},&error);
        if(error)CFRelease(error);
        if(secret && CFDataGetLength(secret)==32){memcpy(out,CFDataGetBytePtr(secret),32);result=CRYPTO_OK;}
        if(secret)CFRelease(secret);
    }
    if(key)CFRelease(key);if(own)CFRelease(own);if(remote)CFRelease(remote);return result;
}}

int crypto_random_os(void *out,size_t len){
    return SecRandomCopyBytes(kSecRandomDefault,len,out)==errSecSuccess?CRYPTO_OK:CRYPTO_INTERNAL;
}
