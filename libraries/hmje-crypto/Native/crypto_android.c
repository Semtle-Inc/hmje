#include "crypto.h"
#include <jni.h>
#include <string.h>
#include <stdio.h>
extern JavaVM *hmje_android_java_vm(void) __attribute__((weak));
typedef struct { JavaVM *vm; JNIEnv *env; int attached,frame; } CryptoEnv;
static int enter(CryptoEnv *c){
    memset(c,0,sizeof(*c));if(!hmje_android_java_vm || !(c->vm=hmje_android_java_vm()))return 0;
    jint r=(*c->vm)->GetEnv(c->vm,(void **)&c->env,JNI_VERSION_1_6);
    if(r==JNI_EDETACHED){if((*c->vm)->AttachCurrentThread(c->vm,&c->env,NULL)!=JNI_OK)return 0;c->attached=1;}
    else if(r!=JNI_OK)return 0;
    if((*c->env)->PushLocalFrame(c->env,256)!=JNI_OK){if((*c->env)->ExceptionCheck(c->env))(*c->env)->ExceptionClear(c->env);if(c->attached)(*c->vm)->DetachCurrentThread(c->vm);return 0;}
    c->frame=1;return 1;
}
static void leave(CryptoEnv *c){if(c->env && (*c->env)->ExceptionCheck(c->env))(*c->env)->ExceptionClear(c->env);if(c->frame)(*c->env)->PopLocalFrame(c->env,NULL);if(c->attached)(*c->vm)->DetachCurrentThread(c->vm);}
static int ok(JNIEnv *e){return !(*e)->ExceptionCheck(e);}
static jbyteArray bytes(JNIEnv *e,const void *p,int n){jbyteArray b=(*e)->NewByteArray(e,n);if(b && n)(*e)->SetByteArrayRegion(e,b,0,n,p);return ok(e)?b:NULL;}
static void wipe_array(JNIEnv *e,jbyteArray b){if(!b)return;unsigned char z[128]={0};jsize n=(*e)->GetArrayLength(e,b);if(!ok(e))return;for(jsize i=0;i<n;i+=128){jsize k=n-i>128?128:n-i;(*e)->SetByteArrayRegion(e,b,i,k,(jbyte *)z);if(!ok(e))break;}}
static jobject named_instance(JNIEnv *e,const char *cls,const char *alg){
    jclass c=(*e)->FindClass(e,cls);if(!c)return NULL;
    char sig[160];int n=snprintf(sig,sizeof(sig),"(Ljava/lang/String;)L%s;",cls);if(n<0 || (size_t)n>=sizeof(sig))return NULL;
    jmethodID m=(*e)->GetStaticMethodID(e,c,"getInstance",sig);if(!m)return NULL;
    jstring name=(*e)->NewStringUTF(e,alg);if(!name)return NULL;
    jobject v=(*e)->CallStaticObjectMethod(e,c,m,name);return ok(e)?v:NULL;
}
static jobject params(JNIEnv *e){
    jobject p=named_instance(e,"java/security/AlgorithmParameters","EC");if(!p)return NULL;
    jclass gs=(*e)->FindClass(e,"java/security/spec/ECGenParameterSpec");if(!gs)return NULL;
    jmethodID ctor=(*e)->GetMethodID(e,gs,"<init>","(Ljava/lang/String;)V");if(!ctor)return NULL;
    jstring name=(*e)->NewStringUTF(e,"secp256r1");if(!name)return NULL;
    jobject spec=(*e)->NewObject(e,gs,ctor,name);if(!spec || !ok(e))return NULL;
    jclass pc=(*e)->GetObjectClass(e,p);if(!pc)return NULL;
    jmethodID init=(*e)->GetMethodID(e,pc,"init","(Ljava/security/spec/AlgorithmParameterSpec;)V");if(!init)return NULL;
    (*e)->CallVoidMethod(e,p,init,spec);if(!ok(e))return NULL;
    jclass ec=(*e)->FindClass(e,"java/security/spec/ECParameterSpec");if(!ec)return NULL;
    jmethodID get=(*e)->GetMethodID(e,pc,"getParameterSpec","(Ljava/lang/Class;)Ljava/security/spec/AlgorithmParameterSpec;");if(!get)return NULL;
    jobject result=(*e)->CallObjectMethod(e,p,get,ec);return ok(e)?result:NULL;
}
static jobject integer(JNIEnv *e,const unsigned char *raw){
    jclass c=(*e)->FindClass(e,"java/math/BigInteger");if(!c)return NULL;
    jmethodID m=(*e)->GetMethodID(e,c,"<init>","(I[B)V");if(!m)return NULL;
    jbyteArray b=bytes(e,raw,32);if(!b)return NULL;
    jobject v=(*e)->NewObject(e,c,m,1,b);if(!ok(e))return NULL;wipe_array(e,b);return ok(e)?v:NULL;
}
static jobject make_key(JNIEnv *e,const unsigned char *raw,int private_key){
    jobject par=params(e);if(!par)return NULL;
    jobject first=integer(e,raw+(private_key?65:1));if(!first)return NULL;
    jobject spec=NULL;
    if(private_key){
        jclass c=(*e)->FindClass(e,"java/security/spec/ECPrivateKeySpec");if(!c)return NULL;
        jmethodID m=(*e)->GetMethodID(e,c,"<init>","(Ljava/math/BigInteger;Ljava/security/spec/ECParameterSpec;)V");if(!m)return NULL;
        spec=(*e)->NewObject(e,c,m,first,par);
    }else{
        jobject y=integer(e,raw+33);if(!y)return NULL;
        jclass point=(*e)->FindClass(e,"java/security/spec/ECPoint");if(!point)return NULL;
        jmethodID ctor=(*e)->GetMethodID(e,point,"<init>","(Ljava/math/BigInteger;Ljava/math/BigInteger;)V");if(!ctor)return NULL;
        jobject p=(*e)->NewObject(e,point,ctor,first,y);if(!p || !ok(e))return NULL;
        jclass c=(*e)->FindClass(e,"java/security/spec/ECPublicKeySpec");if(!c)return NULL;
        jmethodID m=(*e)->GetMethodID(e,c,"<init>","(Ljava/security/spec/ECPoint;Ljava/security/spec/ECParameterSpec;)V");if(!m)return NULL;
        spec=(*e)->NewObject(e,c,m,p,par);
    }
    if(!spec || !ok(e))return NULL;
    jobject factory=named_instance(e,"java/security/KeyFactory","EC");if(!factory)return NULL;
    jclass fc=(*e)->GetObjectClass(e,factory);if(!fc)return NULL;
    jmethodID gen=(*e)->GetMethodID(e,fc,private_key?"generatePrivate":"generatePublic",private_key?"(Ljava/security/spec/KeySpec;)Ljava/security/PrivateKey;":"(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;");if(!gen)return NULL;
    jobject key=(*e)->CallObjectMethod(e,factory,gen,spec);return ok(e)?key:NULL;
}
static int pair_valid(JNIEnv *e,jobject priv,jobject pub){
    jobject sig=named_instance(e,"java/security/Signature","SHA256withECDSA");if(!sig)return 0;
    jclass c=(*e)->GetObjectClass(e,sig);if(!c)return 0;
    jmethodID init=(*e)->GetMethodID(e,c,"initSign","(Ljava/security/PrivateKey;)V");if(!init)return 0;
    (*e)->CallVoidMethod(e,sig,init,priv);if(!ok(e))return 0;
    jmethodID update=(*e)->GetMethodID(e,c,"update","([B)V");if(!update)return 0;
    const char challenge[]="HMJE-P256-key-pair-check-v1";jbyteArray data=bytes(e,challenge,sizeof(challenge)-1);if(!data)return 0;
    (*e)->CallVoidMethod(e,sig,update,data);if(!ok(e))return 0;
    jmethodID sign=(*e)->GetMethodID(e,c,"sign","()[B");if(!sign)return 0;
    jbyteArray signature=(*e)->CallObjectMethod(e,sig,sign);if(!signature || !ok(e))return 0;
    jmethodID verify_init=(*e)->GetMethodID(e,c,"initVerify","(Ljava/security/PublicKey;)V");if(!verify_init)return 0;
    (*e)->CallVoidMethod(e,sig,verify_init,pub);if(!ok(e))return 0;
    (*e)->CallVoidMethod(e,sig,update,data);if(!ok(e))return 0;
    jmethodID verify=(*e)->GetMethodID(e,c,"verify","([B)Z");if(!verify)return 0;
    jboolean result=(*e)->CallBooleanMethod(e,sig,verify,signature);return ok(e) && result;
}
static jbyteArray secret(JNIEnv *e,jobject priv,jobject peer){
    jobject a=named_instance(e,"javax/crypto/KeyAgreement","ECDH");if(!a)return NULL;
    jclass c=(*e)->GetObjectClass(e,a);if(!c)return NULL;
    jmethodID init=(*e)->GetMethodID(e,c,"init","(Ljava/security/Key;)V");if(!init)return NULL;
    (*e)->CallVoidMethod(e,a,init,priv);if(!ok(e))return NULL;
    jmethodID phase=(*e)->GetMethodID(e,c,"doPhase","(Ljava/security/Key;Z)Ljava/security/Key;");if(!phase)return NULL;
    (*e)->CallObjectMethod(e,a,phase,peer,JNI_TRUE);if(!ok(e))return NULL;
    jmethodID generate=(*e)->GetMethodID(e,c,"generateSecret","()[B");if(!generate)return NULL;
    jbyteArray result=(*e)->CallObjectMethod(e,a,generate);return ok(e)?result:NULL;
}
int64_t hmje_crypto_p256_public(const void *p,int64_t n){
    if(!crypto_public_format(p,n))return CRYPTO_INVALID;
    CryptoEnv c;if(!enter(&c))return CRYPTO_UNSUPPORTED;JNIEnv *e=c.env;int r=CRYPTO_KEY_INVALID;
    jobject pub=make_key(e,p,0);if(pub){unsigned char key[97]={4};key[96]=1;jobject priv=make_key(e,key,1);hmje_crypto_wipe(key,sizeof(key));if(priv){jbyteArray v=secret(e,priv,pub);if(v && (*e)->GetArrayLength(e,v)==32 && ok(e)){r=CRYPTO_OK;wipe_array(e,v);}}}
    leave(&c);return r;
}
int64_t hmje_crypto_p256_pair(const void *p,int64_t n){
    if(!crypto_private_format(p,n))return CRYPTO_INVALID;
    CryptoEnv c;if(!enter(&c))return CRYPTO_UNSUPPORTED;JNIEnv *e=c.env;int r=CRYPTO_KEY_INVALID;
    jobject priv=make_key(e,p,1),pub=NULL;if(priv)pub=make_key(e,p,0);if(priv && pub && pair_valid(e,priv,pub))r=CRYPTO_OK;
    leave(&c);return r;
}
int64_t hmje_crypto_p256_agree(const void *p,int64_t n,const void *peer,int64_t pn,void *out,int64_t cap){
    int r=crypto_agree_args(p,n,peer,pn,out,cap);if(r)return r;
    CryptoEnv c;if(!enter(&c))return CRYPTO_UNSUPPORTED;JNIEnv *e=c.env;r=CRYPTO_KEY_INVALID;unsigned char temp[32]={0};jbyteArray v=NULL;
    jobject priv=make_key(e,p,1),pub=NULL,remote=NULL;if(priv)pub=make_key(e,p,0);if(pub)remote=make_key(e,peer,0);
    if(priv && pub && remote && pair_valid(e,priv,pub)){v=secret(e,priv,remote);if(v && (*e)->GetArrayLength(e,v)==32 && ok(e)){(*e)->GetByteArrayRegion(e,v,0,32,(jbyte *)temp);if(ok(e)){memcpy(out,temp,32);r=CRYPTO_OK;}}}
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);wipe_array(e,v);hmje_crypto_wipe(temp,sizeof(temp));leave(&c);return r;
}
static int integer32(JNIEnv *e,jobject integer,unsigned char out[32]){
    jclass c=(*e)->GetObjectClass(e,integer);if(!c)return 0;
    jmethodID method=(*e)->GetMethodID(e,c,"toByteArray","()[B");if(!method)return 0;
    jbyteArray bytes=(*e)->CallObjectMethod(e,integer,method);if(!bytes || !ok(e))return 0;
    jsize n=(*e)->GetArrayLength(e,bytes);if(!ok(e)||n<1||n>33)return 0;
    unsigned char temp[33]={0};(*e)->GetByteArrayRegion(e,bytes,0,n,(jbyte *)temp);int valid=ok(e) && (n!=33 || temp[0]==0);
    if(valid){memset(out,0,32);size_t skip=n==33?1:0;memcpy(out+32-(n-skip),temp+skip,n-skip);}
    if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);wipe_array(e,bytes);hmje_crypto_wipe(temp,sizeof(temp));return valid;
}
int64_t hmje_crypto_p256_generate(void *out,int64_t cap){
    int r=crypto_generate_args(out,cap);if(r)return r;
    CryptoEnv c;if(!enter(&c))return CRYPTO_UNSUPPORTED;JNIEnv *e=c.env;r=CRYPTO_INTERNAL;unsigned char raw[97]={4};
    jobject gen=named_instance(e,"java/security/KeyPairGenerator","EC");if(!gen)goto done;
    jclass gc=(*e)->GetObjectClass(e,gen);if(!gc)goto done;
    jmethodID init=(*e)->GetMethodID(e,gc,"initialize","(Ljava/security/spec/AlgorithmParameterSpec;)V");if(!init)goto done;
    jobject par=params(e);if(!par)goto done;(*e)->CallVoidMethod(e,gen,init,par);if(!ok(e))goto done;
    jmethodID make=(*e)->GetMethodID(e,gc,"generateKeyPair","()Ljava/security/KeyPair;");if(!make)goto done;
    jobject pair=(*e)->CallObjectMethod(e,gen,make);if(!pair || !ok(e))goto done;
    jclass pc=(*e)->GetObjectClass(e,pair);if(!pc)goto done;
    jmethodID getpub=(*e)->GetMethodID(e,pc,"getPublic","()Ljava/security/PublicKey;");if(!getpub)goto done;
    jmethodID getpriv=(*e)->GetMethodID(e,pc,"getPrivate","()Ljava/security/PrivateKey;");if(!getpriv)goto done;
    jobject pub=(*e)->CallObjectMethod(e,pair,getpub);if(!pub || !ok(e))goto done;
    jobject priv=(*e)->CallObjectMethod(e,pair,getpriv);if(!priv || !ok(e))goto done;
    jclass pubc=(*e)->GetObjectClass(e,pub);if(!pubc)goto done;
    jmethodID getw=(*e)->GetMethodID(e,pubc,"getW","()Ljava/security/spec/ECPoint;");if(!getw)goto done;
    jobject point=(*e)->CallObjectMethod(e,pub,getw);if(!point || !ok(e))goto done;
    jclass pointc=(*e)->GetObjectClass(e,point);if(!pointc)goto done;
    jmethodID getx=(*e)->GetMethodID(e,pointc,"getAffineX","()Ljava/math/BigInteger;");if(!getx)goto done;
    jmethodID gety=(*e)->GetMethodID(e,pointc,"getAffineY","()Ljava/math/BigInteger;");if(!gety)goto done;
    jobject x=(*e)->CallObjectMethod(e,point,getx);if(!x || !ok(e))goto done;
    jobject y=(*e)->CallObjectMethod(e,point,gety);if(!y || !ok(e))goto done;
    jclass privc=(*e)->GetObjectClass(e,priv);if(!privc)goto done;
    jmethodID gets=(*e)->GetMethodID(e,privc,"getS","()Ljava/math/BigInteger;");if(!gets)goto done;
    jobject scalar=(*e)->CallObjectMethod(e,priv,gets);if(!scalar || !ok(e))goto done;
    if(integer32(e,x,raw+1)&&integer32(e,y,raw+33)&&integer32(e,scalar,raw+65)&&crypto_private_format(raw,97)&&pair_valid(e,priv,pub)){memcpy(out,raw,97);r=CRYPTO_OK;}
done:hmje_crypto_wipe(raw,sizeof(raw));leave(&c);return r;
}

int crypto_random_os(void *out,size_t len){
    CryptoEnv c;if(!enter(&c))return CRYPTO_UNSUPPORTED;
    JNIEnv *e=c.env;int result=CRYPTO_INTERNAL;jbyteArray buffer=NULL;
    jclass cls=(*e)->FindClass(e,"java/security/SecureRandom");if(!cls)goto done;
    jmethodID ctor=(*e)->GetMethodID(e,cls,"<init>","()V");if(!ctor)goto done;
    jobject rng=(*e)->NewObject(e,cls,ctor);if(!rng || !ok(e))goto done;
    jmethodID next=(*e)->GetMethodID(e,cls,"nextBytes","([B)V");if(!next)goto done;
    buffer=(*e)->NewByteArray(e,(jsize)len);if(!buffer)goto done;
    (*e)->CallVoidMethod(e,rng,next,buffer);if(!ok(e))goto done;
    (*e)->GetByteArrayRegion(e,buffer,0,(jsize)len,out);if(!ok(e))goto done;
    result=CRYPTO_OK;
done:
    if(!ok(e))(*e)->ExceptionClear(e);
    wipe_array(e,buffer);
    if(!ok(e))result=CRYPTO_INTERNAL;
    leave(&c);return result;
}
