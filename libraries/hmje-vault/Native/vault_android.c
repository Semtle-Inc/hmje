#include "vault.h"
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

extern JavaVM *hmje_android_java_vm(void);
extern jobject hmje_android_application_context(JNIEnv *env);
typedef struct {
    JavaVM *vm;
    JNIEnv *env;
    int attached;
    int frame;
} vault_jni;
static int64_t java_error(JNIEnv *e) {
    jthrowable error = (*e)->ExceptionOccurred(e);
    if (!error)
        return VAULT_INTERNAL;
    (*e)->ExceptionClear(e);
    jclass tag = (*e)->FindClass(e, "javax/crypto/AEADBadTagException");
    int integrity = tag && (*e)->IsInstanceOf(e, error, tag);
    if ((*e)->ExceptionCheck(e))
        (*e)->ExceptionClear(e);
    jclass locked = (*e)->FindClass(e, "android/security/keystore/UserNotAuthenticatedException");
    int denied = locked && (*e)->IsInstanceOf(e, error, locked);
    if ((*e)->ExceptionCheck(e))
        (*e)->ExceptionClear(e);
    jclass invalid =
        (*e)->FindClass(e, "android/security/keystore/KeyPermanentlyInvalidatedException");
    denied |= invalid && (*e)->IsInstanceOf(e, error, invalid);
    if ((*e)->ExceptionCheck(e))
        (*e)->ExceptionClear(e);
    if (tag)
        (*e)->DeleteLocalRef(e, tag);
    if (locked)
        (*e)->DeleteLocalRef(e, locked);
    if (invalid)
        (*e)->DeleteLocalRef(e, invalid);
    (*e)->DeleteLocalRef(e, error);
    return integrity ? VAULT_INTEGRITY : (denied ? VAULT_LOCKED : VAULT_IO);
}
static int start_jni(vault_jni *s) {
    memset(s, 0, sizeof(*s));
    s->vm = hmje_android_java_vm();
    if (!s->vm)
        return 0;
    jint r = (*s->vm)->GetEnv(s->vm, (void **)&s->env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if ((*s->vm)->AttachCurrentThread(s->vm, &s->env, NULL) != JNI_OK)
            return 0;
        s->attached = 1;
    } else if (r != JNI_OK)
        return 0;
    if ((*s->env)->PushLocalFrame(s->env, 128) < 0) {
        (*s->env)->ExceptionClear(s->env);
        if (s->attached)
            (*s->vm)->DetachCurrentThread(s->vm);
        return 0;
    }
    s->frame = 1;
    return 1;
}
static void end_jni(vault_jni *s) {
    if (s->frame) {
        if ((*s->env)->ExceptionCheck(s->env))
            (*s->env)->ExceptionClear(s->env);
        (*s->env)->PopLocalFrame(s->env, NULL);
    }
    if (s->attached)
        (*s->vm)->DetachCurrentThread(s->vm);
}
static jmethodID method(JNIEnv *e, jobject object, const char *name, const char *sig) {
    if (!object || (*e)->ExceptionCheck(e))
        return NULL;
    jclass cls = (*e)->GetObjectClass(e, object);
    if (!cls)
        return NULL;
    jmethodID id = (*e)->GetMethodID(e, cls, name, sig);
    (*e)->DeleteLocalRef(e, cls);
    return id;
}
static jobject object_method(JNIEnv *e, jobject o, const char *name, const char *sig) {
    jmethodID m = method(e, o, name, sig);
    return m ? (*e)->CallObjectMethod(e, o, m) : NULL;
}
static jbyteArray bytes(JNIEnv *e, const void *p, size_t n) {
    jbyteArray a = (*e)->NewByteArray(e, (jsize)n);
    if (a && n)
        (*e)->SetByteArrayRegion(e, a, 0, (jsize)n, p);
    return a;
}
static void wipe_array(JNIEnv *e, jbyteArray a) {
    if (!a)
        return;
    unsigned char zero[1024] = {0};
    jsize n = (*e)->GetArrayLength(e, a);
    for (jsize i = 0; i < n; i += 1024) {
        jsize count = n - i;
        if (count > 1024)
            count = 1024;
        (*e)->SetByteArrayRegion(e, a, i, count, (const jbyte *)zero);
    }
}
static int string_utf8(JNIEnv *e, jstring string, char *out, size_t cap) {
    if (!string)
        return 0;
    jmethodID m = method(e, string, "getBytes", "(Ljava/lang/String;)[B");
    if (!m)
        return 0;
    jbyteArray a = (*e)->CallObjectMethod(e, string, m, (*e)->NewStringUTF(e, "UTF-8"));
    if (!a || (*e)->ExceptionCheck(e))
        return 0;
    jsize n = (*e)->GetArrayLength(e, a);
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    (*e)->GetByteArrayRegion(e, a, 0, n, (jbyte *)out);
    out[n] = 0;
    return !(*e)->ExceptionCheck(e) && hmje_vault_utf8_valid(out, n, 0);
}
static int digest_hex(JNIEnv *e, const void *p, size_t n, char out[65]) {
    jclass c = (*e)->FindClass(e, "java/security/MessageDigest");
    if (!c)
        return 0;
    jmethodID get = (*e)->GetStaticMethodID(e, c, "getInstance",
                                            "(Ljava/lang/String;)Ljava/security/MessageDigest;");
    if (!get)
        return 0;
    jobject digest = (*e)->CallStaticObjectMethod(e, c, get, (*e)->NewStringUTF(e, "SHA-256"));
    if (!digest)
        return 0;
    jmethodID finish = method(e, digest, "digest", "([B)[B");
    if (!finish)
        return 0;
    jbyteArray result = (*e)->CallObjectMethod(e, digest, finish, bytes(e, p, n));
    if (!result || (*e)->ExceptionCheck(e) || (*e)->GetArrayLength(e, result) != 32)
        return 0;
    unsigned char hash[32];
    (*e)->GetByteArrayRegion(e, result, 0, 32, (jbyte *)hash);
    vault_hex(hash, 32, out);
    return !(*e)->ExceptionCheck(e);
}
static int open_dir_at(int parent, const char *name) {
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
        return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        return -1;
    }
    if (fchmod(fd, 0700) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int context_path(JNIEnv *e, jobject ctx, const char *name, char out[VAULT_PATH_MAX]) {
    jobject file = object_method(e, ctx, name, "()Ljava/io/File;");
    jstring path = file ? object_method(e, file, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    return string_utf8(e, path, out, VAULT_PATH_MAX);
}
typedef struct {
    int dir;
    int lock;
    unsigned char aad[600];
    size_t aad_size;
    char name[70];
    char alias[96];
} vault_store;
static int64_t open_store(JNIEnv *e, const void *ns, size_t nl, const void *key, size_t kl,
                          vault_store *s) {
    memset(s, 0, sizeof(*s));
    s->dir = -1;
    s->lock = -1;
    jobject ctx = hmje_android_application_context(e);
    if (!ctx)
        return VAULT_UNSUPPORTED;
    char app[256], path[VAULT_PATH_MAX];
    jstring package = object_method(e, ctx, "getPackageName", "()Ljava/lang/String;");
    if (!string_utf8(e, package, app, sizeof(app)) ||
        !context_path(e, ctx, "getNoBackupFilesDir", path))
        return java_error(e);
    for (const char *p = app; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_'))
            return VAULT_INVALID;
    size_t n = strlen(app);
    memcpy(s->aad, "HMJE-VAULT-1", 12);
    s->aad[12] = 0;
    memcpy(s->aad + 13, app, n);
    s->aad[13 + n] = 0;
    memcpy(s->aad + 14 + n, ns, nl);
    size_t scope = 14 + n + nl;
    char hash[65];
    if (!digest_hex(e, s->aad, scope, hash))
        return java_error(e);
    snprintf(s->alias, sizeof(s->alias), "hmje-vault.v1.%s", hash);
    int base = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0)
        return VAULT_IO;
    int top = open_dir_at(base, "hmje-vault");
    close(base);
    if (top < 0)
        return VAULT_IO;
    int version = open_dir_at(top, "v1");
    close(top);
    if (version < 0)
        return VAULT_IO;
    s->dir = open_dir_at(version, hash);
    close(version);
    if (s->dir < 0)
        return VAULT_IO;
    s->aad[scope] = 0;
    memcpy(s->aad + scope + 1, key, kl);
    s->aad_size = scope + 1 + kl;
    if (!digest_hex(e, s->aad, s->aad_size, hash))
        return java_error(e);
    snprintf(s->name, sizeof(s->name), "%s.bin", hash);
    s->lock = openat(s->dir, ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->lock < 0 || flock(s->lock, LOCK_EX) != 0)
        return VAULT_IO;
    return VAULT_OK;
}
static void close_store(vault_store *s) {
    if (s->lock >= 0) {
        flock(s->lock, LOCK_UN);
        close(s->lock);
    }
    if (s->dir >= 0)
        close(s->dir);
}
static jobject store_key(JNIEnv *e, const char *alias, int create, int64_t *status) {
    jclass ks = (*e)->FindClass(e, "java/security/KeyStore");
    if (!ks)
        return NULL;
    jmethodID get = (*e)->GetStaticMethodID(e, ks, "getInstance",
                                            "(Ljava/lang/String;)Ljava/security/KeyStore;");
    if (!get)
        return NULL;
    jobject store =
        (*e)->CallStaticObjectMethod(e, ks, get, (*e)->NewStringUTF(e, "AndroidKeyStore"));
    if (!store)
        return NULL;
    jmethodID load = method(e, store, "load", "(Ljava/io/InputStream;[C)V");
    if (!load)
        return NULL;
    (*e)->CallVoidMethod(e, store, load, NULL, NULL);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    jmethodID key_get = method(e, store, "getKey", "(Ljava/lang/String;[C)Ljava/security/Key;");
    if (!key_get)
        return NULL;
    jstring name = (*e)->NewStringUTF(e, alias);
    jobject key = (*e)->CallObjectMethod(e, store, key_get, name, NULL);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    if (key)
        return key;
    if (!create) {
        *status = VAULT_INTEGRITY;
        return NULL;
    }
    jclass builder = (*e)->FindClass(e, "android/security/keystore/KeyGenParameterSpec$Builder");
    if (!builder)
        return NULL;
    jmethodID init = (*e)->GetMethodID(e, builder, "<init>", "(Ljava/lang/String;I)V");
    if (!init)
        return NULL;
    jobject b = (*e)->NewObject(e, builder, init, name, (jint)3);
    if (!b)
        return NULL;
    jmethodID bits =
        method(e, b, "setKeySize", "(I)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
    jmethodID modes =
        method(e, b, "setBlockModes",
               "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
    jmethodID padding =
        method(e, b, "setEncryptionPaddings",
               "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
    jmethodID random = method(e, b, "setRandomizedEncryptionRequired",
                              "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;");
    jmethodID build = method(e, b, "build", "()Landroid/security/keystore/KeyGenParameterSpec;");
    if (!bits || !modes || !padding || !random || !build)
        return NULL;
    jclass string = (*e)->FindClass(e, "java/lang/String");
    if (!string)
        return NULL;
    jobjectArray mode = (*e)->NewObjectArray(e, 1, string, (*e)->NewStringUTF(e, "GCM"));
    jobjectArray pad = (*e)->NewObjectArray(e, 1, string, (*e)->NewStringUTF(e, "NoPadding"));
    if (!mode || !pad || (*e)->ExceptionCheck(e))
        return NULL;
    (*e)->CallObjectMethod(e, b, bits, (jint)256);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    (*e)->CallObjectMethod(e, b, modes, mode);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    (*e)->CallObjectMethod(e, b, padding, pad);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    (*e)->CallObjectMethod(e, b, random, JNI_TRUE);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    jobject spec = (*e)->CallObjectMethod(e, b, build);
    if (!spec)
        return NULL;
    jclass gen = (*e)->FindClass(e, "javax/crypto/KeyGenerator");
    if (!gen)
        return NULL;
    jmethodID instance = (*e)->GetStaticMethodID(
        e, gen, "getInstance", "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;");
    if (!instance)
        return NULL;
    jobject generator = (*e)->CallStaticObjectMethod(e, gen, instance, (*e)->NewStringUTF(e, "AES"),
                                                     (*e)->NewStringUTF(e, "AndroidKeyStore"));
    if (!generator)
        return NULL;
    jmethodID gen_init =
        method(e, generator, "init", "(Ljava/security/spec/AlgorithmParameterSpec;)V");
    jmethodID generate = method(e, generator, "generateKey", "()Ljavax/crypto/SecretKey;");
    if (!gen_init || !generate)
        return NULL;
    (*e)->CallVoidMethod(e, generator, gen_init, spec);
    if ((*e)->ExceptionCheck(e))
        return NULL;
    return (*e)->CallObjectMethod(e, generator, generate);
}
static int64_t crypt(JNIEnv *e, vault_store *s, int encrypt, const unsigned char *in, size_t size,
                     unsigned char *out, size_t *out_size) {
    int64_t status = VAULT_IO;
    jobject key = store_key(e, s->alias, encrypt, &status);
    if (!key)
        return (*e)->ExceptionCheck(e) ? java_error(e) : status;
    const unsigned char *payload = in;
    size_t payload_size = size;
    jbyteArray iv = NULL;
    if (!encrypt) {
        if (size < 36 || memcmp(in, "HMJV\1\14\0\0", 8) != 0)
            return VAULT_INTEGRITY;
        iv = bytes(e, in + 8, 12);
        payload = in + 20;
        payload_size = size - 20;
    }
    jclass cls = (*e)->FindClass(e, "javax/crypto/Cipher");
    if (!cls)
        return java_error(e);
    jmethodID get =
        (*e)->GetStaticMethodID(e, cls, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;");
    if (!get)
        return java_error(e);
    jobject cipher =
        (*e)->CallStaticObjectMethod(e, cls, get, (*e)->NewStringUTF(e, "AES/GCM/NoPadding"));
    if (!cipher)
        return java_error(e);
    if (encrypt) {
        jmethodID init = method(e, cipher, "init", "(ILjava/security/Key;)V");
        if (!init)
            return java_error(e);
        (*e)->CallVoidMethod(e, cipher, init, (jint)1, key);
    } else {
        jclass gc = (*e)->FindClass(e, "javax/crypto/spec/GCMParameterSpec");
        if (!gc)
            return java_error(e);
        jmethodID ctor = (*e)->GetMethodID(e, gc, "<init>", "(I[B)V");
        if (!ctor)
            return java_error(e);
        jobject spec = (*e)->NewObject(e, gc, ctor, (jint)128, iv);
        if (!spec)
            return java_error(e);
        jmethodID init =
            method(e, cipher, "init",
                   "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V");
        if (!init)
            return java_error(e);
        (*e)->CallVoidMethod(e, cipher, init, (jint)2, key, spec);
    }
    if ((*e)->ExceptionCheck(e))
        return java_error(e);
    jmethodID aad = method(e, cipher, "updateAAD", "([B)V");
    jmethodID finish = method(e, cipher, "doFinal", "([B)[B");
    if (!aad || !finish)
        return java_error(e);
    (*e)->CallVoidMethod(e, cipher, aad, bytes(e, s->aad, s->aad_size));
    if ((*e)->ExceptionCheck(e))
        return java_error(e);
    jbyteArray input = bytes(e, payload, payload_size);
    if (!input)
        return java_error(e);
    jbyteArray result = (*e)->CallObjectMethod(e, cipher, finish, input);
    status = (*e)->ExceptionCheck(e) ? java_error(e) : VAULT_OK;
    if (encrypt)
        wipe_array(e, input);
    if (status != VAULT_OK || !result)
        return status == VAULT_OK ? VAULT_INTERNAL : status;
    jsize n = (*e)->GetArrayLength(e, result);
    size_t prefix = encrypt ? 20 : 0;
    if (n < 0 || (size_t)n + prefix > *out_size) {
        if (!encrypt)
            wipe_array(e, result);
        return VAULT_INTEGRITY;
    }
    if (encrypt) {
        iv = object_method(e, cipher, "getIV", "()[B");
        if (!iv || (*e)->GetArrayLength(e, iv) != 12)
            return VAULT_INTERNAL;
        memcpy(out, "HMJV\1\14\0\0", 8);
        (*e)->GetByteArrayRegion(e, iv, 0, 12, (jbyte *)(out + 8));
    }
    if (n)
        (*e)->GetByteArrayRegion(e, result, 0, n, (jbyte *)(out + prefix));
    if (!encrypt)
        wipe_array(e, result);
    if ((*e)->ExceptionCheck(e)) {
        hmje_vault_wipe(out, (int64_t)(n + prefix));
        return java_error(e);
    }
    *out_size = (size_t)n + prefix;
    return VAULT_OK;
}
static int64_t read_cipher(vault_store *s, unsigned char *data, size_t *size) {
    int fd = openat(s->dir, s->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? VAULT_MISSING : VAULT_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 36 ||
        st.st_size > VAULT_VALUE_MAX + 36) {
        close(fd);
        return VAULT_INTEGRITY;
    }
    size_t need = (size_t)st.st_size, off = 0;
    if (need > *size) {
        close(fd);
        return VAULT_INTEGRITY;
    }
    while (off < need) {
        ssize_t n = read(fd, data + off, need - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return VAULT_IO;
        }
        off += (size_t)n;
    }
    close(fd);
    *size = need;
    return VAULT_OK;
}
static int64_t write_cipher(vault_store *s, const unsigned char *data, size_t size) {
    char iv[25], name[80];
    vault_hex(data + 8, 12, iv);
    snprintf(name, sizeof(name), ".pending-%s-%ld", iv, (long)getpid());
    int fd = openat(s->dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return VAULT_IO;
    size_t off = 0;
    int ok = 1;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = 0;
            break;
        }
        off += (size_t)n;
    }
    if (ok && fsync(fd) != 0)
        ok = 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok && renameat(s->dir, name, s->dir, s->name) != 0)
        ok = 0;
    if (!ok)
        unlinkat(s->dir, name, 0);
    else if (fsync(s->dir) != 0)
        ok = 0;
    return ok ? VAULT_OK : VAULT_IO;
}
int64_t hmje_vault_get(const void *ns, int64_t nl, const void *key, int64_t kl, void *out,
                       int64_t capacity, uint64_t *length) {
    if (length)
        *length = 0;
    if (!length || !vault_names(ns, nl, key, kl) || capacity < 0 || (capacity && !out))
        return VAULT_INVALID;
    vault_jni j;
    if (!start_jni(&j))
        return VAULT_UNSUPPORTED;
    vault_store s;
    int64_t result = open_store(j.env, ns, (size_t)nl, key, (size_t)kl, &s);
    unsigned char *cipher = NULL, *plain = NULL;
    if (result == VAULT_OK) {
        cipher = malloc(VAULT_VALUE_MAX + 36);
        plain = malloc(VAULT_VALUE_MAX);
        if (!cipher || !plain)
            result = VAULT_INTERNAL;
        else {
            size_t n = VAULT_VALUE_MAX + 36;
            result = read_cipher(&s, cipher, &n);
            if (result == VAULT_OK) {
                size_t p = VAULT_VALUE_MAX;
                result = crypt(j.env, &s, 0, cipher, n, plain, &p);
                if (result == VAULT_OK) {
                    if (p > (uint64_t)capacity)
                        result = VAULT_INVALID;
                    else {
                        if (p)
                            memcpy(out, plain, p);
                        *length = p;
                    }
                }
            }
        }
    }
    if (plain) {
        hmje_vault_wipe(plain, VAULT_VALUE_MAX);
        free(plain);
    }
    free(cipher);
    close_store(&s);
    end_jni(&j);
    return result;
}
int64_t hmje_vault_set(const void *ns, int64_t nl, const void *key, int64_t kl, const void *value,
                       int64_t size) {
    if (!vault_names(ns, nl, key, kl) || size < 0 || size > VAULT_VALUE_MAX || (size && !value))
        return VAULT_INVALID;
    vault_jni j;
    if (!start_jni(&j))
        return VAULT_UNSUPPORTED;
    vault_store s;
    int64_t result = open_store(j.env, ns, (size_t)nl, key, (size_t)kl, &s);
    unsigned char *cipher = NULL;
    if (result == VAULT_OK) {
        cipher = malloc(VAULT_VALUE_MAX + 36);
        if (!cipher)
            result = VAULT_INTERNAL;
        else {
            size_t n = VAULT_VALUE_MAX + 36;
            result = crypt(j.env, &s, 1, value, (size_t)size, cipher, &n);
            if (result == VAULT_OK)
                result = write_cipher(&s, cipher, n);
        }
    }
    free(cipher);
    close_store(&s);
    end_jni(&j);
    return result;
}
int64_t hmje_vault_delete(const void *ns, int64_t nl, const void *key, int64_t kl) {
    if (!vault_names(ns, nl, key, kl))
        return VAULT_INVALID;
    vault_jni j;
    if (!start_jni(&j))
        return VAULT_UNSUPPORTED;
    vault_store s;
    int64_t result = open_store(j.env, ns, (size_t)nl, key, (size_t)kl, &s);
    if (result == VAULT_OK) {
        if (unlinkat(s.dir, s.name, 0) != 0)
            result = errno == ENOENT ? VAULT_MISSING : VAULT_IO;
        else if (fsync(s.dir) != 0)
            result = VAULT_IO;
    }
    close_store(&s);
    end_jni(&j);
    return result;
}
int64_t hmje_vault_app_data_path(void *out, int64_t capacity, uint64_t *length) {
    if (length)
        *length = 0;
    if (!out || capacity <= 0 || !length)
        return VAULT_INVALID;
    vault_jni j;
    if (!start_jni(&j))
        return VAULT_UNSUPPORTED;
    int64_t result = VAULT_UNSUPPORTED;
    jobject ctx = hmje_android_application_context(j.env);
    char path[VAULT_PATH_MAX];
    if (ctx && context_path(j.env, ctx, "getFilesDir", path)) {
        size_t n = strlen(path);
        if (n + 11 >= (size_t)capacity || n + 11 >= sizeof(path))
            result = VAULT_INVALID;
        else {
            int base = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            int dir = base < 0 ? -1 : open_dir_at(base, "hmje-data");
            if (base >= 0)
                close(base);
            if (dir < 0)
                result = VAULT_IO;
            else {
                close(dir);
                memcpy(path + n, "/hmje-data", 11);
                n += 10;
                memcpy(out, path, n);
                *length = n;
                result = VAULT_OK;
            }
        }
    } else if ((*j.env)->ExceptionCheck(j.env))
        result = java_error(j.env);
    end_jni(&j);
    return result;
}
