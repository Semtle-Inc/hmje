/* Common HmjeUI ABI 109: capability, owned snapshot publication, real actions. */
#define HMJE_ANDROID_AX_WORDS 18
#define HMJE_ANDROID_AX_MAX_NODES 8192
#define HMJE_ANDROID_AX_MAX_TEXT (1024 * 1024)
typedef struct { u64 identifier, actions; } HmjeAndroidAXNode;
typedef struct { u64 identifier, action; } HmjeAndroidAXAction;
static pthread_mutex_t g_android_ax_lock = PTHREAD_MUTEX_INITIALIZER;
static HmjeAndroidAXNode g_android_ax_nodes[HMJE_ANDROID_AX_MAX_NODES];
static size_t g_android_ax_count;
static HmjeAndroidAXAction g_android_ax_queue[64];
static unsigned g_android_ax_read, g_android_ax_write;

static jstring hmje_android_ax_string(JNIEnv *env, u64 pointer, u64 length) {
    if (!length) return (*env)->NewString(env, NULL, 0);
    if (!pointer || length > HMJE_ANDROID_AX_MAX_TEXT) return NULL;
    const unsigned char *bytes = (const unsigned char *)(uintptr_t)pointer;
    jchar *units = malloc((size_t)length * sizeof(jchar));
    if (!units) return NULL;
    size_t i = 0, count = 0;
    while (i < length) {
        unsigned c = bytes[i++], value, trailing;
        if (c < 0x80) { units[count++] = (jchar)c; continue; }
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; trailing = 1; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; trailing = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; trailing = 3; }
        else { free(units); return NULL; }
        if (trailing > length - i) { free(units); return NULL; }
        unsigned width = trailing;
        while (trailing--) { unsigned tail = bytes[i++]; if ((tail & 0xc0) != 0x80) { free(units); return NULL; } value = (value << 6) | (tail & 63); }
        if ((width == 2 && value < 0x800) || (width == 3 && value < 0x10000)
            || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { free(units); return NULL; }
        if (value < 0x10000) units[count++] = (jchar)value;
        else { value -= 0x10000; units[count++] = (jchar)(0xd800 | (value >> 10)); units[count++] = (jchar)(0xdc00 | (value & 1023)); }
    }
    jstring result = (*env)->NewString(env, units, (jsize)count); free(units); return result;
}

static int hmje_android_ax_permitted(u64 identifier, u64 action) {
    for (size_t i = 0; i < g_android_ax_count; ++i)
        if (g_android_ax_nodes[i].identifier == identifier) return (g_android_ax_nodes[i].actions & action) != 0;
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeAccessibilityAction(JNIEnv *env, jobject activity, jlong identifier, jint action) {
    (void)env; (void)activity;
    if (action <= 0 || (action & (action - 1)) || action > 32) return JNI_FALSE;
    pthread_mutex_lock(&g_android_ax_lock);
    unsigned next = (g_android_ax_write + 1) % 64;
    int ok = next != g_android_ax_read && hmje_android_ax_permitted((u64)identifier, (u64)action);
    if (ok) { g_android_ax_queue[g_android_ax_write] = (HmjeAndroidAXAction){(u64)identifier, (u64)action}; g_android_ax_write = next; }
    pthread_mutex_unlock(&g_android_ax_lock);
    return ok ? JNI_TRUE : JNI_FALSE;
}

i64 hmje_android_ui_accessibility(i64 operation, i64 address, i64 length) {
    if (operation == 0) return 1;
    if (operation == 2) {
        if (!address || length < (i64)sizeof(HmjeAndroidAXAction)) return -22;
        pthread_mutex_lock(&g_android_ax_lock);
        while (g_android_ax_read != g_android_ax_write) {
            HmjeAndroidAXAction action = g_android_ax_queue[g_android_ax_read]; g_android_ax_read = (g_android_ax_read + 1) % 64;
            if (!hmje_android_ax_permitted(action.identifier, action.action)) continue;
            pthread_mutex_unlock(&g_android_ax_lock);
            memcpy((void *)(uintptr_t)(u64)address, &action, sizeof(action)); return sizeof(action);
        }
        pthread_mutex_unlock(&g_android_ax_lock); return 0;
    }
    if (operation != 1) return -38;
    size_t stride = HMJE_ANDROID_AX_WORDS * sizeof(u64);
    if (length < 0 || (u64)length % stride || (u64)length / stride > HMJE_ANDROID_AX_MAX_NODES
        || (length && (!address || ((u64)address & 7)))) return -22;
    if (!g_java_vm) return -38;
    JNIEnv *env = NULL; int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) { if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return -38; attached = 1; }
    else if (state != JNI_OK || !env) return -38;
    i64 result = -22;
    if ((*env)->PushLocalFrame(env, 16) != JNI_OK) { if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm); return -12; }
    pthread_mutex_lock(&g_activity_lock); jobject activity = g_activity ? (*env)->NewLocalRef(env, g_activity) : NULL; pthread_mutex_unlock(&g_activity_lock);
    size_t count = (size_t)length / stride;
    const u64 *records = (const u64 *)(uintptr_t)(u64)address;
    HmjeAndroidAXNode *next = calloc(count ? count : 1, sizeof(*next));
    u64 *keys = calloc(16384, sizeof(u64));
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jlongArray numbers = (*env)->NewLongArray(env, (jsize)(count * HMJE_ANDROID_AX_WORDS));
    jobjectArray labels = string_class ? (*env)->NewObjectArray(env, (jsize)count, string_class, NULL) : NULL;
    jobjectArray values = string_class ? (*env)->NewObjectArray(env, (jsize)count, string_class, NULL) : NULL;
    if (!activity || !next || !keys || !numbers || !labels || !values) { result = -12; goto done; }
    size_t total_text = 0;
    for (size_t i = 0; i < count; ++i) {
        const u64 *r = records + i * HMJE_ANDROID_AX_WORDS;
        if (!r[0] || r[2] < 1 || r[2] > 7 || r[8] > 1000000 || r[9] > 1000000
            || (i64)r[6] < -1000000 || (i64)r[6] > 1000000 || (i64)r[7] < -1000000 || (i64)r[7] > 1000000
            || r[11] > HMJE_ANDROID_AX_MAX_TEXT || (!(r[3] & 32) && r[13] > HMJE_ANDROID_AX_MAX_TEXT)) goto done;
        /* The common wire is preorder: each nonzero parent must already exist.
         * This rejects orphan/self/cyclic graphs before Java tree traversal. */
        if (r[1]) {
            size_t parent = (size_t)((r[1] ^ (r[1] >> 32)) & 16383);
            while (keys[parent] && keys[parent] != r[1]) parent = (parent + 1) & 16383;
            if (!keys[parent]) goto done;
        }
        size_t key = (size_t)((r[0] ^ (r[0] >> 32)) & 16383);
        while (keys[key] && keys[key] != r[0]) key = (key + 1) & 16383;
        if (keys[key]) goto done;
        keys[key] = r[0];
        total_text += (size_t)r[11] + ((r[3] & 32) ? 0 : (size_t)r[13]);
        if (total_text > HMJE_ANDROID_AX_MAX_TEXT) goto done;
        jstring label = hmje_android_ax_string(env, r[10], r[11]);
        jstring value = hmje_android_ax_string(env, (r[3] & 32) ? 0 : r[12], (r[3] & 32) ? 0 : r[13]);
        if (!label || !value) goto done;
        (*env)->SetObjectArrayElement(env, labels, (jsize)i, label); (*env)->SetObjectArrayElement(env, values, (jsize)i, value);
        (*env)->DeleteLocalRef(env, label); (*env)->DeleteLocalRef(env, value);
        u64 clean[HMJE_ANDROID_AX_WORDS]; memcpy(clean, r, sizeof(clean)); memset(clean + 10, 0, 4 * sizeof(u64));
        (*env)->SetLongArrayRegion(env, numbers, (jsize)(i * HMJE_ANDROID_AX_WORDS), HMJE_ANDROID_AX_WORDS, (jlong *)clean);
        next[i] = (HmjeAndroidAXNode){r[0], (r[3] & 1) ? 0 : r[4]};
    }
    if ((*env)->ExceptionCheck(env)) { result = -12; goto done; }
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID publish = (*env)->GetMethodID(env, activity_class, "publishAccessibilitySnapshot", "([J[Ljava/lang/String;[Ljava/lang/String;I)V");
    if (!publish) { result = -38; goto done; }
    (*env)->CallVoidMethod(env, activity, publish, numbers, labels, values, (jint)g_scale);
    if ((*env)->ExceptionCheck(env)) { result = -5; goto done; }
    pthread_mutex_lock(&g_android_ax_lock); memcpy(g_android_ax_nodes, next, count * sizeof(*next)); g_android_ax_count = count; pthread_mutex_unlock(&g_android_ax_lock);
    result = length;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    free(keys); free(next); (*env)->PopLocalFrame(env, NULL);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}
