#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t address;
    uint64_t length;
} hmje_certificate_descriptor;

extern JavaVM *hmje_android_java_vm(void);

static int jni_failed(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

int64_t hmje_network_verify_server_trust(
    int64_t host_address,
    int64_t host_length,
    int64_t descriptors_address,
    int64_t certificate_count
) {
    if (host_address == 0 || host_length <= 0 || host_length > 253 ||
        descriptors_address == 0 || certificate_count <= 0 || certificate_count > 16) {
        return -1;
    }
    JavaVM *vm = hmje_android_java_vm();
    if (vm == NULL) return -1;

    JNIEnv *env = NULL;
    int attached = 0;
    jint environment = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (environment == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return -1;
        attached = 1;
    } else if (environment != JNI_OK || env == NULL) {
        return -1;
    }

    int ok = 0;
    char *host_bytes = (char *)malloc((size_t)host_length + 1);
    if (host_bytes == NULL) goto done;
    memcpy(host_bytes, (const void *)(uintptr_t)host_address, (size_t)host_length);
    host_bytes[host_length] = '\0';
    jstring host = (*env)->NewStringUTF(env, host_bytes);
    free(host_bytes);
    if (host == NULL || jni_failed(env)) goto done;

    jclass certificate_factory_class = (*env)->FindClass(env, "java/security/cert/CertificateFactory");
    jclass byte_stream_class = (*env)->FindClass(env, "java/io/ByteArrayInputStream");
    jclass x509_class = (*env)->FindClass(env, "java/security/cert/X509Certificate");
    if (certificate_factory_class == NULL || byte_stream_class == NULL || x509_class == NULL ||
        jni_failed(env)) goto done;
    jmethodID get_instance = (*env)->GetStaticMethodID(
        env, certificate_factory_class, "getInstance",
        "(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;"
    );
    jmethodID stream_init = (*env)->GetMethodID(env, byte_stream_class, "<init>", "([B)V");
    jmethodID generate_certificate = (*env)->GetMethodID(
        env, certificate_factory_class, "generateCertificate",
        "(Ljava/io/InputStream;)Ljava/security/cert/Certificate;"
    );
    if (get_instance == NULL || stream_init == NULL || generate_certificate == NULL ||
        jni_failed(env)) goto done;
    jstring x509_name = (*env)->NewStringUTF(env, "X.509");
    jobject factory = (*env)->CallStaticObjectMethod(
        env, certificate_factory_class, get_instance, x509_name
    );
    if (factory == NULL || jni_failed(env)) goto done;

    jobjectArray chain = (*env)->NewObjectArray(env, (jsize)certificate_count, x509_class, NULL);
    if (chain == NULL || jni_failed(env)) goto done;
    const hmje_certificate_descriptor *descriptors =
        (const hmje_certificate_descriptor *)(uintptr_t)descriptors_address;
    for (int64_t index = 0; index < certificate_count; ++index) {
        if (descriptors[index].address == 0 || descriptors[index].length == 0 ||
            descriptors[index].length > INT32_MAX) goto done;
        jbyteArray encoded = (*env)->NewByteArray(env, (jsize)descriptors[index].length);
        if (encoded == NULL || jni_failed(env)) goto done;
        (*env)->SetByteArrayRegion(
            env, encoded, 0, (jsize)descriptors[index].length,
            (const jbyte *)(uintptr_t)descriptors[index].address
        );
        jobject stream = (*env)->NewObject(env, byte_stream_class, stream_init, encoded);
        jobject certificate = stream == NULL ? NULL : (*env)->CallObjectMethod(
            env, factory, generate_certificate, stream
        );
        if (certificate == NULL || jni_failed(env)) goto done;
        (*env)->SetObjectArrayElement(env, chain, (jsize)index, certificate);
        if (jni_failed(env)) goto done;
        (*env)->DeleteLocalRef(env, certificate);
        (*env)->DeleteLocalRef(env, stream);
        (*env)->DeleteLocalRef(env, encoded);
    }

    jclass factory_class = (*env)->FindClass(env, "javax/net/ssl/TrustManagerFactory");
    if (factory_class == NULL || jni_failed(env)) goto done;
    jmethodID default_algorithm = (*env)->GetStaticMethodID(
        env, factory_class, "getDefaultAlgorithm", "()Ljava/lang/String;"
    );
    jmethodID factory_get_instance = (*env)->GetStaticMethodID(
        env, factory_class, "getInstance",
        "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;"
    );
    jmethodID factory_init = (*env)->GetMethodID(
        env, factory_class, "init", "(Ljava/security/KeyStore;)V"
    );
    jmethodID get_managers = (*env)->GetMethodID(
        env, factory_class, "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;"
    );
    if (default_algorithm == NULL || factory_get_instance == NULL || factory_init == NULL ||
        get_managers == NULL || jni_failed(env)) goto done;
    jstring algorithm = (*env)->CallStaticObjectMethod(env, factory_class, default_algorithm);
    jobject trust_factory = algorithm == NULL ? NULL : (*env)->CallStaticObjectMethod(
        env, factory_class, factory_get_instance, algorithm
    );
    if (trust_factory == NULL || jni_failed(env)) goto done;
    (*env)->CallVoidMethod(env, trust_factory, factory_init, NULL);
    jobjectArray managers = (*env)->CallObjectMethod(env, trust_factory, get_managers);
    if (managers == NULL || jni_failed(env)) goto done;

    jclass x509_manager_class = (*env)->FindClass(env, "javax/net/ssl/X509TrustManager");
    jobject manager = NULL;
    jsize manager_count = (*env)->GetArrayLength(env, managers);
    for (jsize index = 0; index < manager_count; ++index) {
        jobject candidate = (*env)->GetObjectArrayElement(env, managers, index);
        if (candidate != NULL && (*env)->IsInstanceOf(env, candidate, x509_manager_class)) {
            manager = candidate;
            break;
        }
        if (candidate != NULL) (*env)->DeleteLocalRef(env, candidate);
    }
    if (manager == NULL || jni_failed(env)) goto done;

    jclass extensions_class = (*env)->FindClass(env, "android/net/http/X509TrustManagerExtensions");
    jmethodID extensions_init = extensions_class == NULL ? NULL : (*env)->GetMethodID(
        env, extensions_class, "<init>", "(Ljavax/net/ssl/X509TrustManager;)V"
    );
    jmethodID check_server = extensions_class == NULL ? NULL : (*env)->GetMethodID(
        env, extensions_class, "checkServerTrusted",
        "([Ljava/security/cert/X509Certificate;Ljava/lang/String;Ljava/lang/String;)Ljava/util/List;"
    );
    if (extensions_class == NULL || extensions_init == NULL || check_server == NULL ||
        jni_failed(env)) goto done;
    jobject extensions = (*env)->NewObject(env, extensions_class, extensions_init, manager);
    jobject leaf_certificate = (*env)->GetObjectArrayElement(env, chain, 0);
    jmethodID get_public_key = (*env)->GetMethodID(
        env, x509_class, "getPublicKey", "()Ljava/security/PublicKey;"
    );
    jobject public_key = leaf_certificate == NULL || get_public_key == NULL
        ? NULL
        : (*env)->CallObjectMethod(env, leaf_certificate, get_public_key);
    jclass key_class = (*env)->FindClass(env, "java/security/Key");
    jmethodID get_algorithm = key_class == NULL ? NULL : (*env)->GetMethodID(
        env, key_class, "getAlgorithm", "()Ljava/lang/String;"
    );
    jstring key_algorithm = public_key == NULL || get_algorithm == NULL
        ? NULL
        : (*env)->CallObjectMethod(env, public_key, get_algorithm);
    const char *algorithm_bytes = key_algorithm == NULL
        ? NULL
        : (*env)->GetStringUTFChars(env, key_algorithm, NULL);
    const char *auth_name = algorithm_bytes != NULL && strcmp(algorithm_bytes, "EC") == 0
        ? "ECDHE_ECDSA"
        : "RSA";
    jstring auth_type = (*env)->NewStringUTF(env, auth_name);
    if (algorithm_bytes != NULL) {
        (*env)->ReleaseStringUTFChars(env, key_algorithm, algorithm_bytes);
    }
    jobject verified = extensions == NULL || auth_type == NULL ? NULL : (*env)->CallObjectMethod(
        env, extensions, check_server, chain, auth_type, host
    );
    if (verified != NULL && !jni_failed(env)) ok = 1;

done:
    if (jni_failed(env)) ok = 0;
    if (attached) (*vm)->DetachCurrentThread(vm);
    return ok ? 0 : -1;
}

int64_t hmje_network_verify_server_ecdsa_p256_sha256(
    int64_t certificate_address,
    int64_t certificate_length,
    int64_t digest_address,
    int64_t digest_length,
    int64_t signature_address,
    int64_t signature_length
) {
    if (certificate_address == 0 || certificate_length <= 0 ||
        certificate_length > INT32_MAX || digest_address == 0 ||
        digest_length != 32 || signature_address == 0 ||
        signature_length <= 0 || signature_length > INT32_MAX) return -1;
    JavaVM *vm = hmje_android_java_vm();
    if (vm == NULL) return -1;
    JNIEnv *env = NULL;
    int attached = 0;
    jint environment = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (environment == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return -1;
        attached = 1;
    } else if (environment != JNI_OK || env == NULL) {
        return -1;
    }

    int ok = 0;
    jclass certificate_factory_class = (*env)->FindClass(
        env, "java/security/cert/CertificateFactory"
    );
    jclass byte_stream_class = (*env)->FindClass(env, "java/io/ByteArrayInputStream");
    jmethodID get_factory = certificate_factory_class == NULL ? NULL :
        (*env)->GetStaticMethodID(
            env, certificate_factory_class, "getInstance",
            "(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;"
        );
    jmethodID stream_init = byte_stream_class == NULL ? NULL :
        (*env)->GetMethodID(env, byte_stream_class, "<init>", "([B)V");
    jmethodID generate_certificate = certificate_factory_class == NULL ? NULL :
        (*env)->GetMethodID(
            env, certificate_factory_class, "generateCertificate",
            "(Ljava/io/InputStream;)Ljava/security/cert/Certificate;"
        );
    if (certificate_factory_class == NULL || byte_stream_class == NULL ||
        get_factory == NULL || stream_init == NULL || generate_certificate == NULL ||
        jni_failed(env)) goto verify_done;
    jstring x509_name = (*env)->NewStringUTF(env, "X.509");
    jobject factory = x509_name == NULL ? NULL : (*env)->CallStaticObjectMethod(
        env, certificate_factory_class, get_factory, x509_name
    );
    jbyteArray encoded = (*env)->NewByteArray(env, (jsize)certificate_length);
    if (factory == NULL || encoded == NULL || jni_failed(env)) goto verify_done;
    (*env)->SetByteArrayRegion(
        env, encoded, 0, (jsize)certificate_length,
        (const jbyte *)(uintptr_t)certificate_address
    );
    jobject stream = (*env)->NewObject(env, byte_stream_class, stream_init, encoded);
    jobject certificate = stream == NULL ? NULL : (*env)->CallObjectMethod(
        env, factory, generate_certificate, stream
    );
    if (certificate == NULL || jni_failed(env)) goto verify_done;
    jclass certificate_class = (*env)->FindClass(env, "java/security/cert/Certificate");
    jmethodID get_public_key = certificate_class == NULL ? NULL : (*env)->GetMethodID(
        env, certificate_class, "getPublicKey", "()Ljava/security/PublicKey;"
    );
    jobject public_key = get_public_key == NULL ? NULL : (*env)->CallObjectMethod(
        env, certificate, get_public_key
    );
    jclass signature_class = (*env)->FindClass(env, "java/security/Signature");
    jmethodID signature_instance = signature_class == NULL ? NULL : (*env)->GetStaticMethodID(
        env, signature_class, "getInstance",
        "(Ljava/lang/String;)Ljava/security/Signature;"
    );
    jmethodID init_verify = signature_class == NULL ? NULL : (*env)->GetMethodID(
        env, signature_class, "initVerify", "(Ljava/security/PublicKey;)V"
    );
    jmethodID update = signature_class == NULL ? NULL : (*env)->GetMethodID(
        env, signature_class, "update", "([B)V"
    );
    jmethodID verify = signature_class == NULL ? NULL : (*env)->GetMethodID(
        env, signature_class, "verify", "([B)Z"
    );
    jstring signature_name = (*env)->NewStringUTF(env, "NONEwithECDSA");
    jobject verifier = signature_instance == NULL || signature_name == NULL
        ? NULL
        : (*env)->CallStaticObjectMethod(
            env, signature_class, signature_instance, signature_name
        );
    jbyteArray digest = (*env)->NewByteArray(env, (jsize)digest_length);
    jbyteArray signature = (*env)->NewByteArray(env, (jsize)signature_length);
    if (public_key == NULL || verifier == NULL || init_verify == NULL || update == NULL ||
        verify == NULL || digest == NULL || signature == NULL || jni_failed(env)) {
        goto verify_done;
    }
    (*env)->SetByteArrayRegion(
        env, digest, 0, (jsize)digest_length,
        (const jbyte *)(uintptr_t)digest_address
    );
    (*env)->SetByteArrayRegion(
        env, signature, 0, (jsize)signature_length,
        (const jbyte *)(uintptr_t)signature_address
    );
    (*env)->CallVoidMethod(env, verifier, init_verify, public_key);
    (*env)->CallVoidMethod(env, verifier, update, digest);
    if (!jni_failed(env)) ok = (*env)->CallBooleanMethod(env, verifier, verify, signature);

verify_done:
    if (jni_failed(env)) ok = 0;
    if (attached) (*vm)->DetachCurrentThread(vm);
    return ok ? 0 : -1;
}
