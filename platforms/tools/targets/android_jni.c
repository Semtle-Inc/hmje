/*
 * Android JNI 진입점과 ANativeWindow 렌더링.
 * android_shim.c의 weak 프레임버퍼·입력·자원 훅을 강한 정의로 제공한다.
 *
 * hmje_app_main은 백그라운드 pthread 하나에서 실행한다.
 * ANativeWindow_lock/unlockAndPost는 UI 스레드 전용 API가 아니므로
 * 같은 렌더 스레드에서 창을 잠그고 픽셀을 표시한다.
 *
 * APK 자원은 AAssetManager_open으로 연 AAsset*이다. 압축된 자원에는
 * AAsset_openFileDescriptor를 사용할 수 없으므로 합성 핸들을 AAsset*에
 * 매핑하고 읽기·상태 조회·닫기 훅에서 그 매핑을 사용한다.
 *
 * HMJE 픽셀 0x00RRGGBB의 메모리 순서는 B,G,R,X이며,
 * WINDOW_FORMAT_RGBX_8888은 R,G,B,X이다. present에서 R/B를 교환한다.
 */

























#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pointer_input.h"
#include "legacy_input.h"

#define LOG_TAG "hmjeapp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

typedef long long i64;
typedef unsigned long long u64;

#define HMJE_ANDROID_ENOSYS (-38LL)

/* hmje가 방출한 진입점. tools/targets/android.py가 LLVM IR 텍스트에서 @main →
 * @hmje_app_main 으로 링크 전에 미리 바꿔둔다(lower_llvm.py는 안 건드림). */
extern int hmje_app_main(void);

/* The render thread owns g_window. Java publishes an independently retained
 * window and only surfaceChanged marks it ready for drawing. */
static ANativeWindow *g_window = NULL;
static ANativeWindow *g_published_window = NULL;
static int g_published_ready = 0;
static int g_app_resumed = 0;

i64 hmje_android_app_state(void) {
    return __atomic_load_n(&g_app_resumed, __ATOMIC_ACQUIRE) ? 1 : 3;
}
static int g_app_destroyed = 0;
static u64 g_window_generation = 0, g_render_generation = 0;
static int g_surface_redraw = 0;
static pthread_cond_t g_surface_changed = PTHREAD_COND_INITIALIZER;
static JavaVM *g_java_vm = NULL;
static jobject g_activity = NULL;
static jobject g_asset_manager_owner = NULL;
static i64 g_text_input_client = 0, g_text_input_flags = 0;
static int g_restore_text_input = 0;
static pthread_mutex_t g_activity_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 g_width = 0;
static u64 g_height = 0;
/* Java publishes geometry; only the HMJE render thread replaces its buffers. */
static pthread_mutex_t g_surface_geometry_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 g_pending_width = 0;
static u64 g_pending_height = 0;
static int g_surface_started = 0;
static u64 g_pitch = 0; /* 캔버스(오프스크린 백버퍼)의 바이트 단위 pitch */
static u64 g_scale = 1; /* Android density를 반올림한 물리px/논리dp 배율. */
static unsigned char *g_backbuffer = NULL; /* canonical 0x00RRGGBB, LE u32/px */

/* Android EGL 수명주기와 legacy framebuffer presenter. 정규 UI 경로는 아래
 * android_gles_command_renderer.inc가 같은 context에서 SYS_GPU_COMMAND를 직접
 * 실행한다. 이 presenter는 SemtleOS가 아닌 hosted production의 정상 경로가 아니다. */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLConfig g_egl_config = NULL;
static GLuint g_present_program = 0;
static GLuint g_present_texture = 0;
static GLuint g_present_vao = 0;
static int g_gles_unavailable = 0;

static sem_t g_surface_ready;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static pthread_once_t g_surface_semaphore_once = PTHREAD_ONCE_INIT;
static void initialize_surface_semaphore(void) { sem_init(&g_surface_ready, 0, 0); }
static pthread_mutex_t g_audio_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_permission_changed = PTHREAD_COND_INITIALIZER;
static int g_audio_permission_waiting = 0;
static i64 g_audio_permission_result = 0;
static pthread_mutex_t g_location_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_location_permission_changed = PTHREAD_COND_INITIALIZER;
static int g_location_permission_waiting = 0;
static i64 g_location_permission_result = 0;
static pthread_mutex_t g_camera_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_camera_permission_waiting = 0;
static i64 g_camera_permission_result = 0;

#define HMJE_INPUT_KIND_SHIFT HMJE_LEGACY_INPUT_KIND_SHIFT
#define HMJE_INPUT_KIND_POINTER_MOVE HMJE_LEGACY_INPUT_KIND_POINTER_MOVE
#define HMJE_INPUT_KIND_POINTER_BUTTON HMJE_LEGACY_INPUT_KIND_POINTER_BUTTON
#define HMJE_INPUT_KIND_KEY 0ULL
#define HMJE_INPUT_KIND_TEXT 4ULL
#define HMJE_INPUT_PRESSED_FLAG 256ULL
#define HMJE_INPUT_TEXT_CODEPOINT_MASK 2097151ULL
#define HMJE_INPUT_POINTER_COORD_MASK 16777215ull
#define HMJE_INPUT_POINTER_Y_SHIFT 24
#define HMJE_INPUT_POINTER_PRESSED_FLAG (1ull << 55)

static hmje_pointer_ring g_pointer_samples;
static hmje_legacy_input_ring g_legacy_input;
static pthread_mutex_t g_legacy_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 g_legacy_primary_pointer = UINT64_MAX;

static void legacy_input_push(u64 value) {
    pthread_mutex_lock(&g_legacy_lock);
    hmje_legacy_input_ring_push(&g_legacy_input, value);
    pthread_mutex_unlock(&g_legacy_lock);
}

static void legacy_text_push_codepoint(u64 codepoint) {
    if (codepoint == 0 || codepoint > HMJE_INPUT_TEXT_CODEPOINT_MASK) return;
    legacy_input_push((HMJE_INPUT_KIND_TEXT << HMJE_INPUT_KIND_SHIFT)
                      | (codepoint & HMJE_INPUT_TEXT_CODEPOINT_MASK));
}

static void legacy_key_push(u64 scan_code, int pressed) {
    u64 event = (HMJE_INPUT_KIND_KEY << HMJE_INPUT_KIND_SHIFT) | (scan_code & 0x000f00ffULL);
    if (pressed) event |= HMJE_INPUT_PRESSED_FLAG;
    legacy_input_push(event);
}

static i64 legacy_input_pop(void) {
    pthread_mutex_lock(&g_legacy_lock);
    i64 value = hmje_legacy_input_ring_pop(&g_legacy_input);
    pthread_mutex_unlock(&g_legacy_lock);
    return value;
}

static u64 legacy_pointer_event(u64 kind, u64 x, u64 y, int down) {
    u64 value = (kind << HMJE_INPUT_KIND_SHIFT)
              | (x & HMJE_INPUT_POINTER_COORD_MASK)
              | ((y & HMJE_INPUT_POINTER_COORD_MASK) << HMJE_INPUT_POINTER_Y_SHIFT);
    if (down) value |= HMJE_INPUT_POINTER_PRESSED_FLAG;
    return value;
}

static AAssetManager *g_asset_manager = NULL;

static GLuint compile_gl_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(shader, sizeof(log), &n, log);
        LOGI("GLES shader compile failed: %.*s", (int)n, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* A missing Android Surface suspends UI execution; it is not a successful
 * display operation and is not treated as a permanent GPU failure. GL objects
 * remain in the same context while the render thread waits for the next window. */
static int synchronize_surface_window(void) {
retry_surface:
    pthread_mutex_lock(&g_surface_geometry_lock);
    if (g_window_generation == g_render_generation) {
        pthread_mutex_unlock(&g_surface_geometry_lock);
        return 1;
    }
    int suspended = !g_published_ready;
    if (suspended) LOGI("surface suspended: waiting for a ready window");
    while ((!g_published_ready || !g_app_resumed) && !g_app_destroyed)
        pthread_cond_wait(&g_surface_changed, &g_surface_geometry_lock);
    if (g_app_destroyed) {
        pthread_mutex_unlock(&g_surface_geometry_lock);
        return 0;
    }
    ANativeWindow *next = g_published_window;
    ANativeWindow_acquire(next);
    u64 generation = g_window_generation;
    pthread_mutex_unlock(&g_surface_geometry_lock);

    if (g_egl_context != EGL_NO_CONTEXT) {
        EGLint format = 0;
        eglGetConfigAttrib(g_egl_display, g_egl_config, EGL_NATIVE_VISUAL_ID, &format);
        if (format) ANativeWindow_setBuffersGeometry(next, 0, 0, format);
        EGLSurface replacement = eglCreateWindowSurface(g_egl_display, g_egl_config, next, NULL);
        if (replacement == EGL_NO_SURFACE ||
            !eglMakeCurrent(g_egl_display, replacement, replacement, g_egl_context)) {
            EGLint error = eglGetError();
            if (replacement != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, replacement);
            ANativeWindow_release(next);
            pthread_mutex_lock(&g_surface_geometry_lock);
            int replaced = generation != g_window_generation;
            pthread_mutex_unlock(&g_surface_geometry_lock);
            if (replaced && (error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW)) goto retry_surface;
            LOGI("surface rebind failed: egl=0x%x", error);
            return 0;
        }
        if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
        g_egl_surface = replacement;
    }
    if (g_window) ANativeWindow_release(g_window);
    g_window = next;
    g_render_generation = generation;
    g_surface_redraw = g_surface_started;
    LOGI("surface ready: generation=%llu context-preserved=%d", generation, g_egl_context != EGL_NO_CONTEXT);
    return 1;
}

static int surface_replacement_pending(void) {
    pthread_mutex_lock(&g_surface_geometry_lock);
    int pending = g_window_generation != g_render_generation;
    pthread_mutex_unlock(&g_surface_geometry_lock);
    return pending;
}

static int init_gles_presenter(void) {
    if (!synchronize_surface_window()) return 0;
    if (g_egl_context != EGL_NO_CONTEXT && g_egl_surface != EGL_NO_SURFACE) {
        return 1;
    }
    if (g_gles_unavailable || g_window == NULL) {
        return 0;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        LOGI("GLES presenter: eglInitialize failed error=0x%x", eglGetError());
        g_gles_unavailable = 1;
        return 0;
    }
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &config_count) || config_count < 1) {
        LOGI("GLES presenter: no ES3 window config error=0x%x", eglGetError());
        eglTerminate(display);
        g_gles_unavailable = 1;
        return 0;
    }
    EGLint native_format = 0;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &native_format);
    /* surfaceChanged 이후 HMJE 렌더 스레드에서만 형식을 확정한다. */

    if (native_format != 0) {
        ANativeWindow_setBuffersGeometry(g_window, 0, 0, native_format);
    }
    EGLSurface surface = eglCreateWindowSurface(display, config, g_window, NULL);
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        LOGI("GLES presenter: context/surface creation failed error=0x%x", eglGetError());
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
        g_gles_unavailable = 1;
        return 0;
    }

    static const char *vertex_source =
        "#version 300 es\n"
        "out vec2 vUV;\n"
        "void main(){\n"
        "  vec2 p=vec2((gl_VertexID==1||gl_VertexID==3)?1.0:-1.0,"
        "              (gl_VertexID>=2)?1.0:-1.0);\n"
        "  vec2 uv=p*0.5+0.5;\n"
        "  vUV=vec2(uv.x,1.0-uv.y);\n"
        "  gl_Position=vec4(p,0.0,1.0);\n"
        "}\n";
    static const char *fragment_source =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D uFrame;\n"
        "in vec2 vUV; out vec4 outColor;\n"
        "void main(){ vec4 c=texture(uFrame,vUV); outColor=vec4(c.b,c.g,c.r,1.0); }\n";
    GLuint vs = compile_gl_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = compile_gl_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vs || !fs) {
        g_gles_unavailable = 1;
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(program, sizeof(log), &n, log);
        LOGI("GLES presenter: link failed: %.*s", (int)n, log);
        glDeleteProgram(program);
        g_gles_unavailable = 1;
        return 0;
    }

    GLuint texture = 0, vao = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)g_width, (GLsizei)g_height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenVertexArrays(1, &vao);

    eglSwapInterval(display, 1);
    g_egl_display = display;
    g_egl_surface = surface;
    g_egl_context = context;
    g_egl_config = config;
    g_present_program = program;
    g_present_texture = texture;
    g_present_vao = vao;
    LOGI("GLES presenter ready: EGL %d.%d GL=%s surface=%llux%llu",
         major, minor, glGetString(GL_VERSION),
         (unsigned long long)g_width, (unsigned long long)g_height);
    return 1;
}

/* EGL may acquire the resized native drawable only during swap.  A bounded
 * present retry can request one more UI frame even if the app would be idle. */
static void hmje_android_request_surface_redraw(void) { g_surface_redraw = 1; }

/* 공통 display list를 실행하는 production GLES backend. EGL/window 소유권은 이
 * 번역 단위에 남기기 위해 include로 결합한다. */
#include "android_gles_command_renderer.inc"

static int draw_software_frame(GLsizei drawable_width, GLsizei drawable_height) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawable_width, drawable_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glUseProgram(g_present_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_present_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)g_width, (GLsizei)g_height,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_backbuffer);
    glUniform1i(glGetUniformLocation(g_present_program, "uFrame"), 0);
    glBindVertexArray(g_present_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR;
}

static int present_gles(void) {
    return hmje_gles_present_frame(draw_software_frame, g_width, g_height);
}

/* ---- 합성 핸들 ↔ AAsset* 테이블 ------------------------------------------------------
 * HmjeUI keeps up to 64 (family, size, weight) font faces alive and the icon
 * atlas owns additional assets. 16 slots made later faces fail to open; text
 * using those weights then measured to zero and disappeared on Android. Keep
 * enough room for the complete font cache plus icon and application assets. */
#define MAX_OPEN_ASSETS 128
/* AAsset handles are not POSIX FDs. Keep them beyond the entire int FD domain. */
#define ASSET_HANDLE_BASE INT64_C(0x100000000)
static AAsset *g_open_assets[MAX_OPEN_ASSETS];

static i64 asset_table_insert(AAsset *asset) {
    for (int i = 0; i < MAX_OPEN_ASSETS; i++) {
        if (g_open_assets[i] == NULL) {
            g_open_assets[i] = asset;
            return ASSET_HANDLE_BASE + i;
        }
    }
    return HMJE_ANDROID_ENOSYS;
}

static AAsset *asset_table_lookup(i64 handle) {
    if (handle < ASSET_HANDLE_BASE || handle >= ASSET_HANDLE_BASE + MAX_OPEN_ASSETS) {
        return NULL;
    }
    return g_open_assets[handle - ASSET_HANDLE_BASE];
}

static void asset_table_remove(i64 handle) {
    if (handle >= ASSET_HANDLE_BASE && handle < ASSET_HANDLE_BASE + MAX_OPEN_ASSETS) {
        g_open_assets[handle - ASSET_HANDLE_BASE] = NULL;
    }
}

#include "android_bundle_file.inc"
#include "android_accessibility.inc"

/* ---- android_shim.c 의 weak 훅 오버라이드: 디버그 write 미러링 ------------------------ */

void hmje_android_debug_write(const char *bytes, size_t len) {
    if (len == 0) {
        return;
    }
    char buf[256];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, bytes, n);
    buf[n] = '\0';
    LOGI("stdout: %s", buf);
}

void hmje_android_debug_alloc(u64 size, void *result) {
    LOGI("SYS_ALLOC: size=%llu result=%p", (unsigned long long)size, result);
}

/* ---- android_shim.c 의 weak 훅 오버라이드: 제품 자원(AAsset) ------------------------- */

static i64 hmje_android_open_apk_asset(const char *path) {
    if (g_asset_manager == NULL) {
        return -ENOENT;
    }
    AAsset *asset = AAssetManager_open(g_asset_manager, path, AASSET_MODE_RANDOM);
    if (asset == NULL) {
        return -ENOENT;
    }
    i64 handle = asset_table_insert(asset);
    if (handle <= 0) {
        AAsset_close(asset);
        return -ENOMEM;
    }
    return handle;
}

/* 표준 자원 API: 논리 이름 = APK `assets/` 아래 경로(안드로이드.hmje가 그대로 복사). */
i64 hmje_android_open_resource_asset(const char *name, size_t name_len) {
    (void)name_len;
    return hmje_android_open_apk_asset(name);
}

/* 옛 `/system/fonts/{rel}`: `fonts/{rel}` 자원. 손으로 만든 옛 APK는 폰트를 assets
 * 바로 아래 두었으므로 그 자리도 받는다. */
i64 hmje_android_open_font_asset(const char *rel, size_t rel_len) {
    char path[512];
    if (rel_len > sizeof(path) - sizeof("fonts/")) {
        return HMJE_ANDROID_ENOSYS;
    }
    memcpy(path, "fonts/", sizeof("fonts/") - 1);
    memcpy(path + sizeof("fonts/") - 1, rel, rel_len);
    path[sizeof("fonts/") - 1 + rel_len] = '\0';
    i64 handle = hmje_android_open_apk_asset(path);
    if (handle <= 0) {
        handle = hmje_android_open_apk_asset(rel);
    }
    return handle > 0 ? handle : HMJE_ANDROID_ENOSYS;
}

i64 hmje_android_asset_read(i64 handle, i64 addr, i64 len) {
    AAsset *asset = asset_table_lookup(handle);
    if (asset == NULL) {
        return HMJE_ANDROID_ENOSYS;
    }
    int n = AAsset_read(asset, (void *)(uintptr_t)(u64)addr, (size_t)len);
    if (n < 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    return (i64)n;
}

/* SYS_OBJECT_READ_AT(37): 지정 오프셋에서 읽되 handle의 streaming offset(AAsset 내부 커서)은
 * 바꾸지 않는다(docs/syscall.md READ_AT 계약) — macOS/Windows처럼 pread/위치지정 ReadFile
 * 원콜로 되는 게 아니라, AAsset은 seek 기반 API뿐이라 현재 위치를 저장→목표 오프셋으로
 * seek→읽기→저장해둔 위치로 seek 복원 순으로 흉내낸다. */
i64 hmje_android_asset_read_at(i64 handle, i64 offset, i64 addr, i64 len) {
    AAsset *asset = asset_table_lookup(handle);
    if (asset == NULL || offset < 0 || len < 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    off64_t saved = AAsset_seek64(asset, 0, SEEK_CUR);
    if (saved < 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    if (AAsset_seek64(asset, (off64_t)offset, SEEK_SET) < 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    int n = AAsset_read(asset, (void *)(uintptr_t)(u64)addr, (size_t)len);
    AAsset_seek64(asset, saved, SEEK_SET);
    if (n < 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    return (i64)n;
}

i64 hmje_android_asset_stat(i64 handle, i64 out_addr, i64 out_len) {
    AAsset *asset = asset_table_lookup(handle);
    if (asset == NULL || out_len < 64) {
        return HMJE_ANDROID_ENOSYS;
    }
    /* runtime/user/object.hmje의 객체메타데이터가 읽는 필드만: idx0=식별자,
     * idx3(오프셋24)=크기, idx5(오프셋40)=형식(미사용). */
    u64 record[8];
    memset(record, 0, sizeof(record));
    record[0] = (u64)handle;
    record[3] = (u64)AAsset_getLength64(asset);
    memcpy((void *)(uintptr_t)(u64)out_addr, record, sizeof(record));
    return 0;
}

i64 hmje_android_asset_close(i64 handle) {
    if (handle >= ASSET_HANDLE_BASE && handle < ASSET_HANDLE_BASE + MAX_OPEN_ASSETS) {
        AAsset *asset = asset_table_lookup(handle);
        if (asset == NULL) return -EBADF;
        asset_table_remove(handle);
        AAsset_close(asset);
        return 0;
    }
    /* SYS_CLOSE is shared by AAssets, sockets and regular file descriptors.
     * Never narrow a stale/malformed synthetic handle into an unrelated FD. */
    if (handle < 0 || handle > INT32_MAX) return -EBADF;
    return close((int)handle) == 0 ? 0 : -errno;
}

/* ---- android_shim.c 의 weak 훅 오버라이드: 프레임버퍼/입력 ---------------------------- */

i64 hmje_android_framebuffer_info(i64 out_address, i64 out_length) {
    sem_wait(&g_surface_ready);
    sem_post(&g_surface_ready); /* 딱 한 번만 기다리고 다시 열어둔다(다음 호출도 통과) */
    LOGI("framebuffer_info: out_length=%lld window=%p width=%llu height=%llu backbuffer=%p",
         (long long)out_length, (void *)g_window, (unsigned long long)g_width, (unsigned long long)g_height, (void *)g_backbuffer);
    if (!synchronize_surface_window() || out_length < 48 || g_window == NULL) {
        return HMJE_ANDROID_ENOSYS;
    }
    u64 *out = (u64 *)(uintptr_t)(u64)out_address;
    out[0] = g_width;
    out[1] = g_height;
    out[2] = g_pitch;
    out[3] = 4; /* pixel_bytes */
    out[4] = 1; /* FRAMEBUFFER_FORMAT_XRGB8888 (canonical) */
    out[5] = 1; /* FRAMEBUFFER_FLAG_PRESENT */
    if (out_length >= 88) {
        out[6] = g_scale;
        /* Activity의 SurfaceView가 현재 시스템 바 안쪽에 배치된다.
         * 따라서 Surface 좌표계에서는 추가로 피할 inset이 없다. */
        out[7] = 0;
        out[8] = 0;
        out[9] = 0;
        out[10] = 0;
        return 88;
    }
    if (out_length >= 56) {
        out[6] = g_scale;
        return 56;
    }
    return 48;
}

i64 hmje_android_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y) {
    if (g_backbuffer == NULL || width <= 0 || height <= 0 || pitch <= 0) {
        return 0;
    }
    u64 uw = (u64)width, uh = (u64)height, upitch = (u64)pitch;
    u64 ux = (u64)x, uy = (u64)y;
    if (ux >= g_width || uy >= g_height) {
        return 0;
    }
    u64 copy_w = uw;
    if (ux + copy_w > g_width) {
        copy_w = g_width - ux;
    }
    u64 copy_h = uh;
    if (uy + copy_h > g_height) {
        copy_h = g_height - uy;
    }
    if (copy_w == 0 || copy_h == 0) {
        return 0;
    }
    const unsigned char *src_base = (const unsigned char *)(uintptr_t)(u64)buffer_address;
    i64 total = 0;
    for (u64 row = 0; row < copy_h; row++) {
        const unsigned char *src_row = src_base + row * upitch;
        unsigned char *dst_row = g_backbuffer + (uy + row) * g_pitch + ux * 4;
        size_t bytes = (size_t)(copy_w * 4);
        memcpy(dst_row, src_row, bytes);
        total += (i64)bytes;
    }
    return total;
}

i64 hmje_android_framebuffer_present(i64 rects_address, i64 count) {
    (void)rects_address;
    if (!synchronize_surface_window()) return HMJE_ANDROID_ENOSYS;
    if (count <= 0 || g_window == NULL || g_backbuffer == NULL) {
        return 0;
    }
    if (present_gles()) {
        return 1;
    }
    /* EGL/GLES3가 없거나 초기화에 실패한 기기의 영구 CPU 폴백. */
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(g_window, &buffer, NULL) != 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    static int logged_format = 0;
    if (!logged_format) {
        /* WINDOW_FORMAT_RGBX_8888=2, RGBA_8888=1 — MainActivity.java가 SurfaceHolder.
         * setFormat(RGBX_8888)로 요청한 게 실제로 이 값으로 왔는지 1회만 확인 로그. */
        LOGI("framebuffer_present: locked buffer format=%d (RGBX_8888 expected=2)", buffer.format);
        logged_format = 1;
    }
    u64 copy_w = g_width < (u64)buffer.width ? g_width : (u64)buffer.width;
    u64 copy_h = g_height < (u64)buffer.height ? g_height : (u64)buffer.height;
    /* buffer.stride 는 "픽셀" 단위다(바이트 아님) — WINDOW_FORMAT_RGBX_8888은 4바이트/px. */
    size_t dst_stride_bytes = (size_t)buffer.stride * 4;
    unsigned char *dst_base = (unsigned char *)buffer.bits;
    for (u64 row = 0; row < copy_h; row++) {
        const unsigned char *src_row = g_backbuffer + row * g_pitch; /* canonical: B,G,R,X */
        unsigned char *dst_row = dst_base + row * dst_stride_bytes;  /* RGBX_8888: R,G,B,X */
        for (u64 col = 0; col < copy_w; col++) {
            unsigned char b = src_row[col * 4 + 0];
            unsigned char g = src_row[col * 4 + 1];
            unsigned char r = src_row[col * 4 + 2];
            dst_row[col * 4 + 0] = r;
            dst_row[col * 4 + 1] = g;
            dst_row[col * 4 + 2] = b;
            dst_row[col * 4 + 3] = 0xFF;
        }
    }
    ANativeWindow_unlockAndPost(g_window);
    return 1;
}

static i64 hmje_android_surface_resize_event(void) {
    pthread_mutex_lock(&g_surface_geometry_lock);
    u64 width = g_pending_width, height = g_pending_height;
    pthread_mutex_unlock(&g_surface_geometry_lock);
    if (!width || !height) return 0;
    if (width == g_width && height == g_height) {
        if (!g_surface_redraw) return 0;
        g_surface_redraw = 0;
        return (i64)((5ULL << HMJE_INPUT_KIND_SHIFT) | ((g_scale & 255ULL) << 48)
                 | ((height & 16777215ULL) << 24) | (width & 16777215ULL));
    }
    g_surface_redraw = 0;
    if (width > SIZE_MAX / 4 || height > SIZE_MAX / (width * 4)) return 0;
    unsigned char *pixels = calloc(1, (size_t)(width * height * 4));
    if (!pixels) return 0;
    free(g_backbuffer);
    g_backbuffer = pixels;
    g_width = width;
    g_height = height;
    g_pitch = width * 4;
    /* The software presenter texture is separate from direct-GPU targets;
     * those resize lazily at the next BEGIN.  Both stay on this GL thread. */
    if (g_egl_context != EGL_NO_CONTEXT && g_present_texture) {
        glBindTexture(GL_TEXTURE_2D, g_present_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    LOGI("surface resize: %llux%llu", (unsigned long long)width, (unsigned long long)height);
    return (i64)((5ULL << HMJE_INPUT_KIND_SHIFT) | ((g_scale & 255ULL) << 48)
                 | ((height & 16777215ULL) << 24) | (width & 16777215ULL));
}

i64 hmje_android_input_poll(void) {
    if (!synchronize_surface_window()) return HMJE_ANDROID_ENOSYS;
    /* Synthesize rather than enqueue: even a full input queue cannot lose the
     * only notification for keyboard, rotation, or multi-window geometry. */
    i64 resize = hmje_android_surface_resize_event();
    if (resize) return resize;
    return legacy_input_pop();
}

i64 hmje_android_input_poll2(i64 out_address, i64 out_length) {
    return hmje_pointer_ring_pop(&g_pointer_samples, out_address, out_length);
}

#include "text_input_control.h"
static i64 hmje_android_text_input_apply(i64 client, i64 flags) {
    if (g_java_vm == NULL) return HMJE_ANDROID_ENOSYS;
    JNIEnv *env = NULL;
    int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return HMJE_ANDROID_ENOSYS;
        attached = 1;
    } else if (state != JNI_OK || env == NULL) return HMJE_ANDROID_ENOSYS;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    jclass activity_class = activity == NULL ? NULL : (*env)->GetObjectClass(env, activity);
    jmethodID method = activity_class == NULL ? NULL :
        (*env)->GetMethodID(env, activity_class, "setTextInputConfig", "(JJ)Z");
    jboolean applied = JNI_FALSE;
    if (method != NULL) {
        applied = (*env)->CallBooleanMethod(env, activity, method, (jlong)client, (jlong)flags);
    } else if (activity_class != NULL && (((u64)flags) & ~UINT64_C(3)) == 0
               && ((u64)client) <= (UINT64_MAX >> 2)) {
        /* Retain 104 interoperability with previously generated ordinary Java
         * activities. Secure/multiline CONTROL2 can never take this branch. */
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        jmethodID legacy = (*env)->GetMethodID(env, activity_class, "setTextInputRequest", "(J)V");
        if (legacy != NULL) {
            (*env)->CallVoidMethod(env, activity, legacy, (jlong)(((u64)client << 2) | (u64)flags));
            applied = JNI_TRUE;
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        applied = JNI_FALSE;
    }
    if (applied) {
        pthread_mutex_lock(&g_activity_lock);
        g_text_input_client = client;
        g_text_input_flags = flags;
        pthread_mutex_unlock(&g_activity_lock);
    }
    if (activity_class != NULL) (*env)->DeleteLocalRef(env, activity_class);
    if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return applied ? 1 : HMJE_ANDROID_ENOSYS;
}
i64 hmje_android_text_input_control(i64 request) {
    return hmje_android_text_input_apply((i64)((u64)request >> 2), request & 3);
}
i64 hmje_android_text_input_control2(i64 version, i64 client, i64 flags) {
    int valid = hmje_text_input_validate((u64)version, (u64)client, (u64)flags);
    if (valid) return valid;
    return hmje_android_text_input_apply(client, flags);
}

i64 hmje_android_audio_permission_bridge(i64 action) {
    if (g_java_vm == NULL) return HMJE_ANDROID_ENOSYS;
    JNIEnv *env = NULL;
    int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return HMJE_ANDROID_ENOSYS;
        attached = 1;
    } else if (state != JNI_OK || env == NULL) return HMJE_ANDROID_ENOSYS;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    jclass activity_class = activity == NULL ? NULL : (*env)->GetObjectClass(env, activity);
    i64 result = HMJE_ANDROID_ENOSYS;
    if (activity_class != NULL && action == 0) {
        jmethodID query = (*env)->GetMethodID(env, activity_class, "microphonePermissionStatus", "()I");
        if (query != NULL) result = (i64)(*env)->CallIntMethod(env, activity, query);
    } else if (activity_class != NULL && action == 1) {
        jmethodID request = (*env)->GetMethodID(env, activity_class, "requestMicrophonePermission", "()V");
        if (request != NULL) {
            pthread_mutex_lock(&g_audio_permission_lock);
            g_audio_permission_waiting = 1;
            g_audio_permission_result = 0;
            (*env)->CallVoidMethod(env, activity, request);
            while (g_audio_permission_waiting) {
                pthread_cond_wait(&g_audio_permission_changed, &g_audio_permission_lock);
            }
            result = g_audio_permission_result;
            pthread_mutex_unlock(&g_audio_permission_lock);
        }
    }
    if (activity_class != NULL) (*env)->DeleteLocalRef(env, activity_class);
    if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}

/* ---- hmje 앱 스레드 부트스트랩 --------------------------------------------------------- */

static void *hmje_thread_entry(void *arg) {
    (void)arg;
    LOGI("hmje thread starting hmje_app_main()");
    hmje_app_main();
    return NULL;
}

static void start_hmje_thread_once(void) {
    pthread_t thread;
    int status = pthread_create(&thread, NULL, hmje_thread_entry, NULL);
    if (status != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "HMJE app thread creation failed: %s (%d)", strerror(status), status);
        /* pthread_once cannot retry a failed initializer. Fail visibly instead
         * of leaving the activity alive with no HMJE app thread. */
        _Exit(EXIT_FAILURE);
    }
    pthread_detach(thread);
}

/* ---- JNI 진입점(net.semtle.semtleuidemo.MainActivity, tools/targets/android.py가 생성) --- */

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeInit(JNIEnv *env, jobject thiz, jobject asset_manager, jint scale) {
    if ((*env)->GetJavaVM(env, &g_java_vm) != JNI_OK) {
        g_java_vm = NULL;
    }
    pthread_mutex_lock(&g_activity_lock);
    jobject previous_activity = g_activity;
    g_restore_text_input = previous_activity != NULL;
    g_activity = (*env)->NewGlobalRef(env, thiz);
    if (previous_activity != NULL) (*env)->DeleteGlobalRef(env, previous_activity);
    if (g_asset_manager_owner == NULL) {
        g_asset_manager_owner = (*env)->NewGlobalRef(env, asset_manager);
        g_asset_manager = AAssetManager_fromJava(env, asset_manager);
    }
    pthread_mutex_unlock(&g_activity_lock);
    pthread_once(&g_surface_semaphore_once, initialize_surface_semaphore);
    g_scale = scale > 0 ? (u64)scale : 1;
    LOGI("nativeInit: asset manager ready scale=%llu", (unsigned long long)g_scale);
}

/* state: 1=resumed, 2=paused, 3=destroyed. Surface loss is independent: a
 * paused activity may temporarily keep its Surface, but the render thread must
 * still sleep until both lifecycle and surface are ready. */
JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeLifecycle(
    JNIEnv *env, jobject thiz, jint state
) {
    (void)env;
    (void)thiz;
    pthread_mutex_lock(&g_surface_geometry_lock);
    if (state == 1) __atomic_store_n(&g_app_resumed, 1, __ATOMIC_RELEASE);
    if (state == 2) __atomic_store_n(&g_app_resumed, 0, __ATOMIC_RELEASE);
    if (state == 3) {
        __atomic_store_n(&g_app_resumed, 0, __ATOMIC_RELEASE);
        g_app_destroyed = 1;
    }
    pthread_cond_broadcast(&g_surface_changed);
    pthread_mutex_unlock(&g_surface_geometry_lock);
    if (state == 3)
        legacy_input_push(6ULL << HMJE_INPUT_KIND_SHIFT);
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeTextCodepoint(
    JNIEnv *env, jobject thiz, jint codepoint
) {
    (void)env;
    (void)thiz;
    /* Common text sessions handle hard line breaks through the Enter key path.
     * IMEs also commit LF inside a document (paste/prediction/multiline input),
     * so preserve it instead of sending a control character that UI filters. */
    if (codepoint == '\n') {
        legacy_key_push(28, 1);
        legacy_key_push(28, 0);
        return;
    }
    legacy_text_push_codepoint((u64)(uint32_t)codepoint);
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeTextKey(
    JNIEnv *env, jobject thiz, jint scan_code, jboolean pressed
) {
    (void)env;
    (void)thiz;
    legacy_key_push((u64)(uint32_t)scan_code, pressed == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeAudioPermissionResult(
    JNIEnv *env, jobject thiz, jint status
) {
    (void)env; (void)thiz;
    pthread_mutex_lock(&g_audio_permission_lock);
    g_audio_permission_result = (i64)status;
    g_audio_permission_waiting = 0;
    pthread_cond_broadcast(&g_audio_permission_changed);
    pthread_mutex_unlock(&g_audio_permission_lock);
}

/* hmje-location의 Android 다리. 오디오 권한 다리와 같은 모양이지만 HMJE 실행기
 * 스레드를 막지 않도록 권한 요청은 시작(1)과 결과 조회(2)로 나눈다. 상태 값은
 * MainActivity.locationPermissionStatus()의 0 미결정/1 거부/2 사용 중/3 항상이다.
 *   action 0: 권한 상태 조회
 *   action 1: 권한 요청 시작(즉시 0 반환; 결과는 action 2로 조회)
 *   action 2: 요청 결과 조회(-1 대기 중, 아니면 상태)
 *   action 3: 위치 갱신 시작(argument = 최소 간격 ms)
 *   action 4: 위치 갱신 중지
 *   action 5: 단발 위치 요청
 * 위치 표본은 MainActivity의 LocationListener가 nativeLocationSample로 올리고,
 * hmje_android_location_set_sink로 등록한 C 함수에 그대로 넘긴다(HMJE 호출 없음). */
typedef void (*hmje_android_location_sink)(
    double latitude, double longitude, double accuracy, double speed, i64 time_ms, i64 mock);
static hmje_android_location_sink g_location_sink = NULL;

void hmje_android_location_set_sink(hmje_android_location_sink sink) {
    pthread_mutex_lock(&g_location_permission_lock);
    g_location_sink = sink;
    pthread_mutex_unlock(&g_location_permission_lock);
}

i64 hmje_android_location_bridge(i64 action, i64 argument) {
    if (action == 2) {
        pthread_mutex_lock(&g_location_permission_lock);
        i64 pending = g_location_permission_waiting ? -1 : g_location_permission_result;
        pthread_mutex_unlock(&g_location_permission_lock);
        return pending;
    }
    if (g_java_vm == NULL) return HMJE_ANDROID_ENOSYS;
    JNIEnv *env = NULL;
    int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return HMJE_ANDROID_ENOSYS;
        attached = 1;
    } else if (state != JNI_OK || env == NULL) return HMJE_ANDROID_ENOSYS;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    jclass activity_class = activity == NULL ? NULL : (*env)->GetObjectClass(env, activity);
    i64 result = HMJE_ANDROID_ENOSYS;
    if (activity_class != NULL && action == 0) {
        jmethodID query = (*env)->GetMethodID(env, activity_class, "locationPermissionStatus", "()I");
        if (query != NULL) result = (i64)(*env)->CallIntMethod(env, activity, query);
    } else if (activity_class != NULL && action == 1) {
        jmethodID request = (*env)->GetMethodID(env, activity_class, "requestLocationPermission", "()V");
        if (request != NULL) {
            pthread_mutex_lock(&g_location_permission_lock);
            g_location_permission_waiting = 1;
            g_location_permission_result = 0;
            pthread_mutex_unlock(&g_location_permission_lock);
            (*env)->CallVoidMethod(env, activity, request);
            result = 0;
        }
    } else if (activity_class != NULL && action == 3) {
        jmethodID start = (*env)->GetMethodID(env, activity_class, "startLocationUpdates", "(J)V");
        if (start != NULL) { (*env)->CallVoidMethod(env, activity, start, (jlong)argument); result = 0; }
    } else if (activity_class != NULL && action == 4) {
        jmethodID stop = (*env)->GetMethodID(env, activity_class, "stopLocationUpdates", "()V");
        if (stop != NULL) { (*env)->CallVoidMethod(env, activity, stop); result = 0; }
    } else if (activity_class != NULL && action == 5) {
        jmethodID single = (*env)->GetMethodID(env, activity_class, "requestSingleLocation", "()V");
        if (single != NULL) { (*env)->CallVoidMethod(env, activity, single); result = 0; }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        result = HMJE_ANDROID_ENOSYS;
    }
    if (activity_class != NULL) (*env)->DeleteLocalRef(env, activity_class);
    if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeLocationPermissionResult(
    JNIEnv *env, jobject thiz, jint status
) {
    (void)env; (void)thiz;
    pthread_mutex_lock(&g_location_permission_lock);
    g_location_permission_result = (i64)status;
    g_location_permission_waiting = 0;
    pthread_cond_broadcast(&g_location_permission_changed);
    pthread_mutex_unlock(&g_location_permission_lock);
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeLocationSample(
    JNIEnv *env, jobject thiz, jdouble latitude, jdouble longitude, jdouble accuracy,
    jdouble speed, jlong time_ms, jboolean mock
) {
    (void)env; (void)thiz;
    pthread_mutex_lock(&g_location_permission_lock);
    hmje_android_location_sink sink = g_location_sink;
    pthread_mutex_unlock(&g_location_permission_lock);
    if (sink != NULL) sink(latitude, longitude, accuracy, speed, (i64)time_ms, mock ? 1 : 0);
}

/* hmje-camera의 Android 권한 다리. 위치 다리와 같이 HMJE 실행기 스레드를 막지 않도록
 * 시작(1)과 결과 조회(2)를 나눈다. MainActivity의 camera* 메서드는 `필요 기능은
 * 카메라`일 때만 생성되므로, 없으면 GetMethodID가 실패해 ENOSYS를 돌려준다.
 *   action 0: 권한 상태 조회(0 미결정, 1 거부, 2 허용)
 *   action 1: 권한 요청 시작(즉시 0 반환)
 *   action 2: 요청 결과 조회(-1 대기 중, 아니면 상태)
 * 촬영 자체는 라이브러리 Java(HmjeCameraBridge)가 Activity 클래스 로더로 수행한다. */
i64 hmje_android_camera_bridge(i64 action) {
    if (action == 2) {
        pthread_mutex_lock(&g_camera_permission_lock);
        i64 pending = g_camera_permission_waiting ? -1 : g_camera_permission_result;
        pthread_mutex_unlock(&g_camera_permission_lock);
        return pending;
    }
    if (g_java_vm == NULL) return HMJE_ANDROID_ENOSYS;
    JNIEnv *env = NULL;
    int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return HMJE_ANDROID_ENOSYS;
        attached = 1;
    } else if (state != JNI_OK || env == NULL) return HMJE_ANDROID_ENOSYS;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    jclass activity_class = activity == NULL ? NULL : (*env)->GetObjectClass(env, activity);
    i64 result = HMJE_ANDROID_ENOSYS;
    if (activity_class != NULL && action == 0) {
        jmethodID query = (*env)->GetMethodID(env, activity_class, "cameraPermissionStatus", "()I");
        if (query != NULL) result = (i64)(*env)->CallIntMethod(env, activity, query);
    } else if (activity_class != NULL && action == 1) {
        jmethodID request = (*env)->GetMethodID(env, activity_class, "requestCameraPermission", "()V");
        if (request != NULL) {
            pthread_mutex_lock(&g_camera_permission_lock);
            g_camera_permission_waiting = 1;
            g_camera_permission_result = 0;
            pthread_mutex_unlock(&g_camera_permission_lock);
            (*env)->CallVoidMethod(env, activity, request);
            result = 0;
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (action == 1) {
            pthread_mutex_lock(&g_camera_permission_lock);
            g_camera_permission_waiting = 0;
            pthread_mutex_unlock(&g_camera_permission_lock);
        }
        result = HMJE_ANDROID_ENOSYS;
    }
    if (activity_class != NULL) (*env)->DeleteLocalRef(env, activity_class);
    if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeCameraPermissionResult(
    JNIEnv *env, jobject thiz, jint status
) {
    (void)env; (void)thiz;
    pthread_mutex_lock(&g_camera_permission_lock);
    g_camera_permission_result = (i64)status;
    g_camera_permission_waiting = 0;
    pthread_mutex_unlock(&g_camera_permission_lock);
}

/* 프로젝트 외부 구현이 Android 시스템 서비스를 JNI로 호출해야 할 때 쓰는 최소 host
 * 경계다. JNIEnv는 스레드 로컬이라 보관하지 않고 JavaVM만 제공한다. 호출자는 자기
 * worker/HMJE 스레드를 AttachCurrentThread한 뒤 작업이 끝나면 직접 detach한다. */
JavaVM *hmje_android_java_vm(void) {
    return g_java_vm;
}

/* UI-based platform SDKs (for example CredentialManager) require an Activity,
 * not ApplicationContext. Return a caller-owned local reference on its attached
 * thread. The provider must check lifecycle state again on the main thread;
 * this reference keeps the object alive, not its window/resumed lifecycle. */
jobject hmje_android_current_activity(JNIEnv *env) {
    if (env == NULL) return NULL;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
        return NULL;
    }
    return activity;
}

/* Public native-library boundary. env belongs to the calling attached thread.
 * Returns an owned JNI local reference to ApplicationContext, or NULL without
 * a pending exception. The caller must DeleteLocalRef (or PopLocalFrame).
 * No Activity/global-reference address is exposed to external libraries. */
jobject hmje_android_application_context(JNIEnv *env) {
    if (env == NULL) return NULL;
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    if (activity == NULL) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return NULL;
    }
    jobject context = NULL;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID get_context = cls == NULL ? NULL : (*env)->GetMethodID(
        env, cls, "getApplicationContext", "()Landroid/content/Context;");
    if (get_context != NULL) context = (*env)->CallObjectMethod(env, activity, get_context);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (context != NULL) (*env)->DeleteLocalRef(env, context);
        context = NULL;
    }
    if (cls != NULL) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return context;
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativePointerSample(
    JNIEnv *env, jobject thiz,
    jlong time_us, jlong pointer_id, jlong x, jlong y,
    jint pressure, jint altitude, jint azimuth,
    jint tool, jint phase, jint buttons, jint changed_button,
    jlong scroll_x, jlong scroll_y, jlong flags
) {
    (void)env;
    (void)thiz;
    hmje_pointer_record record;
    memset(&record, 0, sizeof(record));
    record.phase = (u64)phase;
    record.pointer_id = (u64)pointer_id;
    record.x = (i64)x;
    record.y = (i64)y;
    record.time_us = (u64)time_us;
    record.buttons = (u64)(uint32_t)buttons;
    record.changed_button = (u64)(uint32_t)changed_button;
    record.pressure = (u64)(uint32_t)pressure;
    record.altitude = (u64)(uint32_t)altitude;
    record.azimuth = (u64)(uint32_t)azimuth;
    record.scroll_x = (i64)scroll_x;
    record.scroll_y = (i64)scroll_y;
    record.tool = (u64)(uint32_t)tool;
    record.flags = (u64)flags;
    hmje_pointer_ring_push(&g_pointer_samples, record);

    u64 identity = (u64)pointer_id;
    if (record.phase == HMJE_POINTER_PHASE_DOWN && g_legacy_primary_pointer == UINT64_MAX) {
        g_legacy_primary_pointer = identity;
    }
    if (identity == g_legacy_primary_pointer) {
        u64 ux = record.x < 0 ? 0u : (u64)record.x;
        u64 uy = record.y < 0 ? 0u : (u64)record.y;
        legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_MOVE, ux, uy, 0));
        if (record.phase == HMJE_POINTER_PHASE_DOWN) {
            legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_BUTTON, ux, uy, 1));
        } else if (record.phase == HMJE_POINTER_PHASE_UP ||
                   record.phase == HMJE_POINTER_PHASE_CANCEL) {
            legacy_input_push(legacy_pointer_event(HMJE_INPUT_KIND_POINTER_BUTTON, ux, uy, 0));
            g_legacy_primary_pointer = UINT64_MAX;
        }
    }
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeSurfaceCreated(JNIEnv *env, jobject thiz, jobject surface) {
    (void)thiz;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == NULL) {
        LOGI("nativeSurfaceCreated: ANativeWindow_fromSurface failed");
        return;
    }
    pthread_mutex_lock(&g_surface_geometry_lock);
    if (g_published_window) ANativeWindow_release(g_published_window);
    g_published_window = window;
    g_published_ready = 0;
    ++g_window_generation;
    pthread_mutex_unlock(&g_surface_geometry_lock);
    LOGI("nativeSurfaceCreated: window acquired, waiting for surfaceChanged for final geometry");
    /* 여기서는 절대 hmje 스레드를 안 깨운다 — SurfaceHolder.Callback 계약상 surfaceCreated
     * 직후엔 아직 버퍼 지오메트리가 확정되지 않았을 수 있고(Android 공식 가이드도
     * surfaceChanged에서 그리기 시작을 권장), 실기/에뮬레이터 검증 중 이 시점에 바로
     * ANativeWindow_lock을 태우면(백그라운드 스레드가 즉시 present를 부르므로) 무작위
     * 시스템 스레드가 "FORTIFY: pthread_mutex_lock called on a destroyed mutex"로 죽는
     * 크래시가 재현됐다 — SurfaceFlinger가 초기 버퍼를 아직 binder IPC로 협상 중인
     * 타이밍과 경합하는 것으로 추정된다. surfaceChanged까지 기다리니 사라졌다(아래 참고). */
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeSurfaceChanged(JNIEnv *env, jobject thiz, jint width, jint height) {
    (void)env;
    (void)thiz;
    if (width <= 0 || height <= 0) return;
    pthread_mutex_lock(&g_surface_geometry_lock);
    if (g_published_window == NULL) {
        pthread_mutex_unlock(&g_surface_geometry_lock);
        return;
    }
    g_published_ready = 1;
    pthread_cond_broadcast(&g_surface_changed);
    g_pending_width = (u64)width;
    g_pending_height = (u64)height;
    pthread_mutex_unlock(&g_surface_geometry_lock);
    pthread_mutex_lock(&g_activity_lock);
    int restore_input = g_restore_text_input;
    i64 input_client = g_text_input_client, input_flags = g_text_input_flags;
    g_restore_text_input = 0;
    pthread_mutex_unlock(&g_activity_lock);
    if (restore_input) hmje_android_text_input_apply(input_client, input_flags);
    if (g_surface_started) return;
    g_width = (u64)width;
    g_height = (u64)height;
    g_pitch = g_width * 4;
    g_backbuffer = calloc(1, (size_t)(g_pitch * g_height));
    if (!g_backbuffer) return;
    g_surface_started = 1;
    LOGI("nativeSurfaceChanged: %llux%llu, starting hmje thread", (unsigned long long)g_width, (unsigned long long)g_height);

    sem_post(&g_surface_ready);
    pthread_once(&g_thread_once, start_hmje_thread_once);
}

JNIEXPORT void JNICALL
Java_net_semtle_semtleuidemo_MainActivity_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    pthread_mutex_lock(&g_surface_geometry_lock);
    if (g_published_window != NULL) ANativeWindow_release(g_published_window);
    g_published_window = NULL;
    g_published_ready = 0;
    ++g_window_generation;
    pthread_mutex_unlock(&g_surface_geometry_lock);
}

/* Read current Resources.Configuration rather than the theme at APK creation. */
i64 hmje_android_system_theme(void) {
    if (g_java_vm == NULL) return 0;
    JNIEnv *env = NULL;
    int attached = 0;
    jint state = (*g_java_vm)->GetEnv(g_java_vm, (void **)&env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_java_vm)->AttachCurrentThread(g_java_vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    } else if (state != JNI_OK || env == NULL) return 0;
    if ((*env)->PushLocalFrame(env, 8) < 0) {
        (*env)->ExceptionClear(env);
        if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
        return 0;
    }
    pthread_mutex_lock(&g_activity_lock);
    jobject activity = g_activity == NULL ? NULL : (*env)->NewLocalRef(env, g_activity);
    pthread_mutex_unlock(&g_activity_lock);
    i64 result = 0;
    if (activity != NULL) {
        jclass activity_class = (*env)->GetObjectClass(env, activity);
        jmethodID get_resources = (*env)->GetMethodID(env, activity_class, "getResources", "()Landroid/content/res/Resources;");
        jobject resources = get_resources == NULL ? NULL : (*env)->CallObjectMethod(env, activity, get_resources);
        if (resources != NULL && !(*env)->ExceptionCheck(env)) {
            jclass resources_class = (*env)->GetObjectClass(env, resources);
            jmethodID get_config = (*env)->GetMethodID(env, resources_class, "getConfiguration", "()Landroid/content/res/Configuration;");
            jobject config = get_config == NULL ? NULL : (*env)->CallObjectMethod(env, resources, get_config);
            if (config != NULL && !(*env)->ExceptionCheck(env)) {
                jclass config_class = (*env)->GetObjectClass(env, config);
                jfieldID mode_field = (*env)->GetFieldID(env, config_class, "uiMode", "I");
                if (mode_field != NULL) {
                    jint mode = (*env)->GetIntField(env, config, mode_field) & 0x30;
                    result = mode == 0x20 ? 2 : (mode == 0x10 ? 1 : 0);
                }
            }
        }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); result = 0; }
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*g_java_vm)->DetachCurrentThread(g_java_vm);
    return result;
}
