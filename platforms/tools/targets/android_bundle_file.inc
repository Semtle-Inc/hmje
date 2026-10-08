/* APK read-only assets participate in the HMJE POSIX file API through real fds.
 * Normal filesystem access wins. Only missing relative, read-only files fall back.
 * AAsset may be compressed, so materialize it into an unlinked app-private file;
 * pread/lseek/fstat/dup/close retain their ordinary POSIX semantics. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>

int __real_open(const char *, int, ...);
int __real_openat(int, const char *, int, ...);
jobject hmje_android_application_context(JNIEnv *);

/* Android SELinux denies reopening appdomain_tmpfs memfds as O_RDONLY. A
 * mode-0600 cacheDir file can be reopened read-only and immediately unlinked,
 * giving a real immutable-by-descriptor file without a persistent asset cache. */
static int hmje_android_asset_temp(char path[PATH_MAX]) {
    if (!g_java_vm) { errno = ENODEV; return -1; }
    JNIEnv *env = NULL; int attached = 0, result = -1;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) { errno = ENODEV; return -1; }
        attached = 1;
    } else if (state != JNI_OK || !env) { errno = ENODEV; return -1; }
    if ((*env)->PushLocalFrame(env, 8) != JNI_OK) { errno = ENOMEM; goto detach; }
    jobject context = hmje_android_application_context(env);
    jclass cls = context ? (*env)->GetObjectClass(env, context) : NULL;
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "getCacheDir", "()Ljava/io/File;") : NULL;
    jobject directory = method ? (*env)->CallObjectMethod(env, context, method) : NULL;
    jclass file_cls = directory ? (*env)->GetObjectClass(env, directory) : NULL;
    jmethodID absolute = file_cls ? (*env)->GetMethodID(env, file_cls, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    jstring name = absolute ? (*env)->CallObjectMethod(env, directory, absolute) : NULL;
    const char *bytes = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    if (bytes) {
        int size = snprintf(path, PATH_MAX, "%s/.hmje-asset-XXXXXX", bytes);
        (*env)->ReleaseStringUTFChars(env, name, bytes);
        if (size > 0 && size < PATH_MAX) result = mkostemp(path, O_CLOEXEC);
        else errno = ENAMETOOLONG;
    } else errno = EIO;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
detach:
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}

static int hmje_android_asset_path(const char *path, int flags) {
    if (!path || !path[0] || path[0] == '/' || (flags & O_ACCMODE) != O_RDONLY
        || (flags & (O_CREAT | O_TRUNC | O_EXCL | O_DIRECTORY | O_PATH))) return 0;
    const char *segment = path;
    for (const char *p = path;; ++p) {
        if ((size_t)(p - path) >= 4096 || *p == '\\') return 0;
        if (*p == '/' || *p == '\0') {
            size_t size = (size_t)(p - segment);
            if (!size || (size == 1 && segment[0] == '.')
                || (size == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (!*p) return 1;
            segment = p + 1;
        }
    }
}

static int hmje_android_asset_fd(const char *path, int flags) {
    if (!g_asset_manager || !hmje_android_asset_path(path, flags)) { errno = ENOENT; return -1; }
    AAsset *asset = AAssetManager_open(g_asset_manager, path, AASSET_MODE_STREAMING);
    if (!asset) { errno = ENOENT; return -1; }
    char path_buffer[PATH_MAX];
    int fd = hmje_android_asset_temp(path_buffer);
    if (fd < 0) { int e = errno; AAsset_close(asset); errno = e; return -1; }
    char bytes[16384];
    int count, error = 0;
    while ((count = AAsset_read(asset, bytes, sizeof(bytes))) > 0) {
        int offset = 0;
        while (offset < count) {
            ssize_t written = write(fd, bytes + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = errno ? errno : EIO; break; }
            offset += (int)written;
        }
        if (error) break;
    }
    if (count < 0) error = EIO;
    AAsset_close(asset);
    int readonly = -1;
    if (!error) {
        readonly = __real_open(path_buffer, O_RDONLY | O_NOFOLLOW | (flags & O_CLOEXEC));
        if (readonly < 0) error = errno;
    }
    if (unlink(path_buffer) < 0 && !error) {
        error = errno;
        if (readonly >= 0) { close(readonly); readonly = -1; }
    }
    close(fd);
    if (error) errno = error;
    return readonly;
}

int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    int result = __real_open(path, flags, mode);
    if (result >= 0 || errno != ENOENT || !hmje_android_asset_path(path, flags)) return result;
    return hmje_android_asset_fd(path, flags);
}

int __wrap_openat(int directory, const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    int result = __real_openat(directory, path, flags, mode);
    if (result >= 0 || errno != ENOENT || directory != AT_FDCWD
        || !hmje_android_asset_path(path, flags)) return result;
    return hmje_android_asset_fd(path, flags);
}
