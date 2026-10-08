#include "external_url.h"
#include <jni.h>
#include <string.h>

extern JavaVM *hmje_android_java_vm(void);
extern jobject hmje_android_application_context(JNIEnv *env);

int64_t hmje_ui_open_external_url(const uint8_t *bytes, uint64_t length) {
    if (bytes == NULL || length == 0 || length > 4096) return 0;
    char url[4097];
    memcpy(url, bytes, (size_t)length);
    url[length] = 0;
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) return 0;
    JavaVM *vm = hmje_android_java_vm();
    if (vm == NULL) return 0;
    JNIEnv *e = NULL;
    int attached = 0;
    jint rc = (*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return 0;
        attached = 1;
    } else if (rc != JNI_OK) {
        return 0;
    }
    int64_t result = 0;
    if ((*e)->PushLocalFrame(e, 16) != JNI_OK) goto detach;
    jclass uriClass = (*e)->FindClass(e, "android/net/Uri");
    if (uriClass == NULL) goto done;
    jmethodID parse = (*e)->GetStaticMethodID(e, uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (parse == NULL) goto done;
    jstring text = (*e)->NewStringUTF(e, url);
    if (text == NULL) goto done;
    jobject uri = (*e)->CallStaticObjectMethod(e, uriClass, parse, text);
    if (uri == NULL || (*e)->ExceptionCheck(e)) goto done;
    jclass intentClass = (*e)->FindClass(e, "android/content/Intent");
    if (intentClass == NULL) goto done;
    jmethodID ctor = (*e)->GetMethodID(e, intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    if (ctor == NULL) goto done;
    jstring action = (*e)->NewStringUTF(e, "android.intent.action.VIEW");
    if (action == NULL) goto done;
    jobject intent = (*e)->NewObject(e, intentClass, ctor, action, uri);
    if (intent == NULL) goto done;
    jmethodID addFlags = (*e)->GetMethodID(e, intentClass, "addFlags", "(I)Landroid/content/Intent;");
    if (addFlags == NULL) goto done;
    (*e)->CallObjectMethod(e, intent, addFlags, (jint)0x10000000); /* FLAG_ACTIVITY_NEW_TASK */
    if ((*e)->ExceptionCheck(e)) goto done;
    jobject context = hmje_android_application_context(e);
    if (context == NULL) goto done;
    jclass contextClass = (*e)->GetObjectClass(e, context);
    jmethodID start = (*e)->GetMethodID(e, contextClass, "startActivity", "(Landroid/content/Intent;)V");
    if (start == NULL) goto done;
    (*e)->CallVoidMethod(e, context, start, intent);
    if (!(*e)->ExceptionCheck(e)) result = 1;
done:
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    (*e)->PopLocalFrame(e, NULL);
detach:
    if (attached) (*vm)->DetachCurrentThread(vm);
    return result;
}
