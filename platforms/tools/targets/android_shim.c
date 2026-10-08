/*
 * android_shim.c — hmje 앱을 Android(Bionic) 위에서 돌릴 때의 시스템콜 구현.
 *
 * Android APK는 JNI로 Java가 부르는 .so이므로 별도 진입점을 제공한다. SYS_* 번호
 * 계약(runtime/user/syscall.hmje ← kernel/api/syscall.hmje)과 Bionic이 POSIX 상당 부분을
 * 제공한다는 사실 자체는 같아서, EXIT/WRITE/READ(폴백)/ALLOC/FREE/TIME/SLEEP/YIELD/
 * PROCESS_ID/VM_RESERVE/VM_RELEASE 구현 로직은 posix_host_shim.c와 사실상 동일하다.
 *
 * ── 두 단계 빌드 ─────────────────────────────────────────────────────────────────────
 * 이 파일 하나로 두 산출물을 만든다(tools/targets/android.py):
 *   1) android-exe  : 진짜 main()이 있는 플레인 aarch64-linux-android 실행 파일.
 *                      adb push + adb shell 로 직접 실행 — 컴파일러 타깃 + Bionic 링크가
 *                      맞는지 확인한다.
 *   2) android-apk  : main()이 hmje_app_main으로 리네임되어 libhmjeapp.so 안에 들어가고,
 *                      android_jni.c(JNI/Surface)가 실제 진입점을 제공한다.
 *
 * 두 산출물이 같은 이 파일을 링크하되 프레임버퍼/입력/폰트 훅 4+3개는 weak 심볼 기본값
 * (전부 -38, "이 빌드엔 없음")로 둔다 — android-exe는 그 훅들을 아예 안 부르니 기본값으로
 * 충분하고, android-apk는 android_jni.c가 강한 정의로 덮어써서 실제 Surface/AAsset 구현을
 * 제공한다. 그래서 이 파일 하나가 양쪽 빌드에서 안전하게 재사용된다.
 *
 * ── 구현된 syscall ──────────────────────────────────────────────────────────────────
 *   0  SYS_EXIT                   → exit(code)
 *   1  SYS_WRITE                  → write(fd, addr, len)
 *   2  SYS_TIME_MS / 60 TIME_US   → 단조 시계
 *   3  SYS_SLEEP_MS / 61 SLEEP_US → nanosleep
 *   4  SYS_ALLOC / 6 SYS_FREE     → low-address arena free-list
 *   5  SYS_READ                   → hmje_android_asset_read 훅(기본은 POSIX read 폴백)
 *   13 SYS_PROCESS_ID             → getpid
 *   15 SYS_YIELD                  → sched_yield
 *   18/19 SYS_VM_RESERVE/RELEASE  → mmap/munmap(anonymous)
 *   20 SYS_OBJECT_OPEN_GRAPH_WALK → "/system/fonts/{name}" 만 hmje_android_open_font_asset
 *                                   훅으로 라우팅(= 제품 자원 `fonts/{name}`, hmje_resource_open과
 *                                   같은 원천)
 *   21 SYS_CLOSE                  → hmje_android_asset_close 훅
 *   22 SYS_OBJECT_STAT            → hmje_android_asset_stat 훅
 *   37 SYS_OBJECT_READ_AT         → hmje_android_asset_read_at 훅(기본은 POSIX pread 폴백,
 *                                   android-apk는 android_jni.c가 AAsset_seek64로 흉내냄) —
 *                                   폰트 로딩의 위치 지정 읽기도 이 경로를 쓴다.
 *   54/55/62 SYS_FRAMEBUFFER_*    → hmje_android_framebuffer_* 훅
 *   56 SYS_INPUT_POLL             → hmje_android_input_poll 훅
 *   85/86/87 SYS_SOCKET_*         → Bionic TCP connect/send/recv
 *
 * 스텁(HMJE_ANDROID_ENOSYS=-38): 그 외 전부(IPC/부트모듈/디버그/객체 그래프 나머지) — 다른
 * 두 플랫폼과 같은 범위.
 */








#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SYS_EXIT 0
#define SYS_WRITE 1
#define SYS_TIME_MS 2
#define SYS_SLEEP_MS 3
#define SYS_ALLOC 4
#define SYS_READ 5
#define SYS_FREE 6
#define SYS_PROCESS_ID 13
#define SYS_YIELD 15
#define SYS_VM_RESERVE 18
#define SYS_VM_RELEASE 19
#define SYS_OBJECT_OPEN_GRAPH_WALK 20
#define SYS_CLOSE 21
#define SYS_OBJECT_STAT 22
#define SYS_OBJECT_READ_AT 37
#define SYS_FRAMEBUFFER_INFO 54
#define SYS_FRAMEBUFFER_BLIT 55
#define SYS_INPUT_POLL 56
#define SYS_TIME_US 60
#define SYS_SLEEP_US 61
#define SYS_FRAMEBUFFER_PRESENT 62
#define SYS_SOCKET_CONNECT 85
#define SYS_SOCKET_SEND 86
#define SYS_SOCKET_RECV 87
#define SYS_GPU_COMMAND 90
#define SYS_WALL_TIME_S 95
#define SYS_WALL_TIME_MS 114
#define SYS_SECURE_RANDOM_FILL 115
#define SYS_LOCAL_DATETIME 116
#define SYS_INPUT_POLL2 98
#define SYS_ENV_READ 101
#define SYS_UI_SYSTEM_THEME 110
#define SYS_TEXT_INPUT_CONTROL 104
#define SYS_TEXT_INPUT_CONTROL2 111
#define SYS_UI_WINDOW_CONFIG 112
#define SYS_UI_WINDOW 117
#define SYS_UI_CLIPBOARD 113
#define SYS_UI_ACCESSIBILITY 109
#define SYS_AUDIO_CAPTURE_OPEN 105
#define SYS_AUDIO_CAPTURE_READ 106
#define SYS_AUDIO_CAPTURE_CONTROL 107
#define SYS_AUDIO_CAPTURE_PERMISSION 108

#define HMJE_ANDROID_ENOSYS (-38LL)

#define ERR_SOCKET_NOT_CONNECTED 107
#define ERR_SOCKET_TIMEOUT 110
#define ERR_SOCKET_CONN_REFUSED 111
#define ERR_SOCKET_HOST_NOT_FOUND 113
#define ERR_SOCKET_WOULD_BLOCK 11
#define ERR_ENV_NOT_FOUND 2
#define SOCKET_RECV_NONBLOCK 1
#define SOCKET_DEFAULT_TIMEOUT_MS 10000

typedef long long i64;
typedef unsigned long long u64;

/* Hosted worker-pool ABI: online logical CPUs when no performance class is exposed. */
u64 hmje_platform_performance_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (u64)count : 1;
}


/* ---- 플랫폼 훅(weak 기본값, android_jni.c가 android-apk 빌드에서 강한 정의로 덮어씀) ---- */

/* Optional host appearance. Headless/Linux hosts retain light unless explicitly set. */
__attribute__((weak)) i64 hmje_android_ui_accessibility(i64 op, i64 address, i64 length) { (void)op; (void)address; (void)length; return HMJE_ANDROID_ENOSYS; }
__attribute__((weak)) i64 hmje_android_system_theme(void) { return 0; }
__attribute__((weak)) i64 hmje_android_app_state(void) { return 1; }

__attribute__((weak)) i64 hmje_android_framebuffer_info(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y) {
    (void)buffer_address;
    (void)width;
    (void)height;
    (void)pitch;
    (void)x;
    (void)y;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_framebuffer_present(i64 rects_address, i64 count) {
    (void)rects_address;
    (void)count;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_input_poll(void) {
    return 0; /* 이벤트 없음 — 다른 두 플랫폼과 같은 스텁 범위(터치 미배선) */
}
__attribute__((weak)) i64 hmje_android_input_poll2(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return 0;
}
__attribute__((weak)) i64 hmje_android_text_input_control2(i64 version, i64 client, i64 flags) {
    (void)version; (void)client; (void)flags;
    return HMJE_ANDROID_ENOSYS; /* No insecure keyboard fallback. */
}
__attribute__((weak)) i64 hmje_android_text_input_control(i64 active) {
    (void)active;
    return 0;
}
__attribute__((weak)) i64 hmje_android_audio_capture_open(i64 config_address, i64 config_length) {
    (void)config_address; (void)config_length;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_audio_capture_read(i64 handle, i64 output_address, i64 output_capacity, i64 metadata_address, i64 metadata_length) {
    (void)handle; (void)output_address; (void)output_capacity;
    (void)metadata_address; (void)metadata_length;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_audio_capture_control(i64 handle, i64 command) {
    (void)handle; (void)command;
    return HMJE_ANDROID_ENOSYS;
}
__attribute__((weak)) i64 hmje_android_audio_capture_permission(i64 action) {
    (void)action;
    return HMJE_ANDROID_ENOSYS;
}

/* ---- 제품 자원: 표준 자원 API의 Android 원천 ----------------------------------------
 * name/name_len = 검증한 논리 이름(`fonts/Pretendard-Regular.otf`). android-apk에서는
 * android_jni.c가 AAssetManager(APK `assets/<이름>`)로 강한 정의를 준다. 이 기본
 * 구현은 android-exe의 `<실행 파일>.assets/<이름>`을 연다(안드로이드.hmje
 * `안드로이드EXE만들기`가 그 자리에 복사한다). 성공 시 >0 핸들, 없으면 -ENOENT. */
__attribute__((weak)) i64 hmje_android_open_resource_asset(const char *name, size_t name_len) {
    (void)name_len;
    char executable[2048];
    ssize_t executable_size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (executable_size <= 0 || (size_t)executable_size >= sizeof(executable)) {
        return -ENOENT;
    }
    executable[executable_size] = '\0';
    char full[4096];
    int written = snprintf(full, sizeof(full), "%s.assets/%s", executable, name);
    if (written <= 0 || (size_t)written >= sizeof(full)) {
        return -ENOENT;
    }
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -ENOENT;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -ENOENT;
    }
    return (i64)fd;
}

static int hmje_android_resource_name_valid(const char *name, size_t name_len) {
    if (name == NULL || name_len == 0 || name_len > 1024) {
        return 0;
    }
    size_t component_start = 0;
    for (size_t i = 0; i <= name_len; ++i) {
        char byte = i < name_len ? name[i] : '/';
        if (byte == '\0' || byte == '\\' || byte == ':') {
            return 0;
        }
        if (byte != '/') {
            continue;
        }
        size_t component_len = i - component_start;
        if (component_len == 0 ||
            (component_len == 1 && name[component_start] == '.') ||
            (component_len == 2 && name[component_start] == '.' && name[component_start + 1] == '.')) {
            return 0;
        }
        component_start = i + 1;
    }
    return 1;
}

/* 표준 자원 API의 hosted 원천(library/Sources/표준호스트/resource). */
i64 hmje_resource_open(const char *name, u64 name_len) {
    if (!hmje_android_resource_name_valid(name, (size_t)name_len)) {
        return -EINVAL;
    }
    char copy[1025];
    memcpy(copy, name, (size_t)name_len);
    copy[name_len] = '\0';
    return hmje_android_open_resource_asset(copy, (size_t)name_len);
}

/* rel/rel_len = "/system/fonts/" 다음 상대 파일명(옛 폰트 graph walk). `fonts/{rel}`
 * 자원과 같다. 성공 시 >0 핸들, 실패 시 -38. android_jni.c가 옛 APK 배치(assets 뿌리의
 * 폰트)까지 받는 강한 정의를 준다. */
__attribute__((weak)) i64 hmje_android_open_font_asset(const char *rel, size_t rel_len) {
    char name[1025];
    if (rel_len == 0 || rel_len + 6 > 1024) {
        return HMJE_ANDROID_ENOSYS;
    }
    memcpy(name, "fonts/", 6);
    memcpy(name + 6, rel, rel_len);
    name[6 + rel_len] = '\0';
    if (!hmje_android_resource_name_valid(name, 6 + rel_len)) {
        return HMJE_ANDROID_ENOSYS;
    }
    i64 handle = hmje_android_open_resource_asset(name, 6 + rel_len);
    return handle > 0 ? handle : HMJE_ANDROID_ENOSYS;
}
/* 핸들이 hmje_android_open_font_asset이 준 합성 핸들이면 AAsset_read/getLength/close로
 * 라우팅해야 한다(APK 안 zip 엔트리라 진짜 POSIX fd가 아님 — macOS/iOS처럼 "핸들=진짜 fd라
 * SYS_READ가 공짜로 맞는다" 트릭이 여기선 안 통한다). 기본(weak) 구현은 진짜 fd라고 가정하고
 * POSIX 폴백만 한다 — android-exe 빌드에선 폰트 핸들 자체가 안 생기니 이 경로를 안 탄다.
 */
__attribute__((weak)) i64 hmje_android_asset_read(i64 handle, i64 addr, i64 len) {
    return (i64)read((int)handle, (void *)(uintptr_t)(u64)addr, (size_t)len);
}
/* SYS_OBJECT_READ_AT(37): 객체내용읽기(runtime/user/object.hmje)가 쓰는
 * 위치 지정 읽기이며 폰트 로딩에도 쓰인다. 합성 핸들(AAsset)에서는 asset_read처럼 real fd를
 * 가정할 수 없어서 android_jni.c가 AAsset_seek64로 강한 정의를 덮어쓴다. 기본(weak) 구현은
 * hmje_android_asset_read와 같은 이유로 real fd를 가정한 POSIX pread 폴백만 한다. */
__attribute__((weak)) i64 hmje_android_asset_read_at(i64 handle, i64 offset, i64 addr, i64 len) {
    ssize_t n = pread((int)handle, (void *)(uintptr_t)(u64)addr, (size_t)len, (off_t)offset);
    return n < 0 ? HMJE_ANDROID_ENOSYS : (i64)n;
}
__attribute__((weak)) i64 hmje_android_asset_stat(i64 handle, i64 out_addr, i64 out_len) {
    if (out_len < 64) {
        return HMJE_ANDROID_ENOSYS;
    }
    struct stat st;
    if (fstat((int)handle, &st) != 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    u64 record[8];
    memset(record, 0, sizeof(record));
    record[0] = (u64)handle;
    record[3] = (u64)st.st_size;
    memcpy((void *)(uintptr_t)(u64)out_addr, record, sizeof(record));
    return 0;
}
__attribute__((weak)) i64 hmje_android_asset_close(i64 handle) {
    close((int)handle);
    return 0;
}

/* ---- 단조 시계 ---------------------------------------------------------- */

static i64 monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (i64)ts.tv_sec * 1000000000LL + (i64)ts.tv_nsec;
}

static i64 wall_time_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    return (i64)ts.tv_sec;
}

static i64 wall_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -(i64)errno;
    if (ts.tv_sec < 0 || (u64)ts.tv_sec > (INT64_MAX / 1000ULL)) return -75;
    return (i64)ts.tv_sec * 1000LL + (i64)ts.tv_nsec / 1000000LL;
}

/* time_t and struct tm layouts belong to this OS boundary. */
static i64 local_datetime_at_seconds(i64 seconds) {
    time_t instant = (time_t)seconds;
    if ((i64)instant != seconds) return -75;
    struct tm value;
    if (localtime_r(&instant, &value) == NULL) return -5;
    i64 year = (i64)value.tm_year + 1900;
    if (year < 1 || year > 9999) return -75;
    return (((((year * 100 + value.tm_mon + 1) * 100 + value.tm_mday) * 100
              + value.tm_hour) * 100 + value.tm_min) * 100 + value.tm_sec);
}

static i64 local_datetime_packed(void) {
    time_t now = time(NULL);
    if (now == (time_t)-1) return -5;
    return local_datetime_at_seconds((i64)now);
}

static i64 secure_random_fill(i64 address, i64 length) {
    if (address == 0 || length != 8) return -22;
    unsigned char *out = (unsigned char *)(uintptr_t)(u64)address;
    size_t done = 0;
#if __ANDROID_API__ < 28
    int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) return -(i64)errno;
#endif
    while (done < 8) {
#if __ANDROID_API__ >= 28
        ssize_t count = getrandom(out + done, 8 - done, 0);
#else
        ssize_t count = read(random_fd, out + done, 8 - done);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            i64 error = count < 0 ? -(i64)errno : -5;
#if __ANDROID_API__ < 28
            close(random_fd);
#endif
            return error;
        }
        done += (size_t)count;
    }
#if __ANDROID_API__ < 28
    close(random_fd);
#endif
    return 8;
}

int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms);

static i64 sleep_ns(i64 ns) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000LL);
    req.tv_nsec = (long)(ns % 1000000000LL);
    while (nanosleep(&req, &req) != 0) {
    }
    return 0;
}

/* HMJE 할당 래퍼는 반환값을 부호 있는 i64로 해석하여 <=0이면 실패로 본다.
 * Android/Bionic의 힙·mmap 주소에는 63번 비트가 설정될 수 있으므로
 * 이 shim은 mmap 힌트로 부호 비트가 없는 낮은 주소를 할당한다.
 * 성공한 주소가 음수로 해석되어 주소(0)으로 바뀌지 않게 하는 경계다. */







static void *mmap_low_address(size_t size) {
    static const u64 hints[] = {
        0x100000000ULL, 0x200000000ULL, 0x300000000ULL, 0x400000000ULL,
        0x500000000ULL, 0x600000000ULL, 0x700000000ULL,
    };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        void *addr = mmap((void *)(uintptr_t)hints[i], size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (addr == MAP_FAILED) {
            continue;
        }
        if (((u64)(uintptr_t)addr & 0x8000000000000000ULL) == 0) {
            return addr;
        }
        munmap(addr, size); /* 힌트를 무시하고 높은 주소를 줬다 — 버리고 다음 힌트 재시도 */
    }
    /* 마지막 수단: 힌트 없이 시도(대개 여기까지 안 옴, 위 힌트들이 보통 먹힌다) */
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED || ((u64)(uintptr_t)addr & 0x8000000000000000ULL) != 0) {
        return NULL;
    }
    return addr;
}

/* ---- SYS_VM_RESERVE/RELEASE -------------------------------------------- */

struct vm_reservation {
    void *addr;
    size_t size;
    struct vm_reservation *next;
};
static struct vm_reservation *vm_reservations = NULL;

static i64 vm_reserve(u64 size) {
    if (size == 0) {
        return 0;
    }
    void *addr = mmap_low_address((size_t)size);
    if (addr == NULL) {
        return 0;
    }
    struct vm_reservation *node = malloc(sizeof(*node));
    if (node == NULL) {
        munmap(addr, (size_t)size);
        return 0;
    }
    node->addr = addr;
    node->size = (size_t)size;
    node->next = vm_reservations;
    vm_reservations = node;
    return (i64)(u64)(uintptr_t)addr;
}

/* ---- SYS_ALLOC: 낮은 주소 arena free-list ------------------------------
 * Bionic malloc 주소를 HMJE의 부호 있는 syscall 반환값으로 쓸 수 없어 낮은
 * mmap arena를 유지한다. 정렬된 블록 목록에서 first-fit, split, 인접 빈
 * 블록 coalescing을 해 할당·해제 계약을 만족한다. */


#define HMJE_ANDROID_ARENA_SIZE (256u * 1024u * 1024u)

struct android_heap_block {
    size_t size;
    struct android_heap_block *next;
    int free;
    int padding;
    u64 reserved;
};

static unsigned char *g_arena_base = NULL;
static struct android_heap_block *g_arena_blocks = NULL;
static pthread_mutex_t g_arena_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t android_heap_align(size_t size) {
    return (size + 15u) & ~(size_t)15u;
}

static i64 android_alloc(u64 size) {
    if (size == 0 || (u64)(size_t)size != size || (size_t)size > HMJE_ANDROID_ARENA_SIZE) {
        return 0;
    }
    pthread_mutex_lock(&g_arena_lock);
    if (g_arena_base == NULL) {
        g_arena_base = (unsigned char *)mmap_low_address(HMJE_ANDROID_ARENA_SIZE);
        if (g_arena_base == NULL) {
            pthread_mutex_unlock(&g_arena_lock);
            return 0;
        }
        g_arena_blocks = (struct android_heap_block *)g_arena_base;
        g_arena_blocks->size = HMJE_ANDROID_ARENA_SIZE - sizeof(*g_arena_blocks);
        g_arena_blocks->next = NULL;
        g_arena_blocks->free = 1;
        g_arena_blocks->padding = 0;
        g_arena_blocks->reserved = 0;
    }
    size_t aligned_size = android_heap_align((size_t)size);
    struct android_heap_block *block = g_arena_blocks;
    while (block != NULL && (!block->free || block->size < aligned_size)) {
        block = block->next;
    }
    if (block == NULL) {
        pthread_mutex_unlock(&g_arena_lock);
        return 0;
    }
    if (block->size >= aligned_size + sizeof(*block) + 16u) {
        struct android_heap_block *split = (struct android_heap_block *)
            ((unsigned char *)(block + 1) + aligned_size);
        split->size = block->size - aligned_size - sizeof(*block);
        split->next = block->next;
        split->free = 1;
        split->padding = 0;
        split->reserved = 0;
        block->size = aligned_size;
        block->next = split;
    }
    block->free = 0;
    void *result = (void *)(block + 1);
    pthread_mutex_unlock(&g_arena_lock);
    return (i64)(u64)(uintptr_t)result;
}

static void android_free(u64 address) {
    if (address == 0) return;
    pthread_mutex_lock(&g_arena_lock);
    unsigned char *pointer = (unsigned char *)(uintptr_t)address;
    if (g_arena_base == NULL || pointer <= g_arena_base ||
        pointer >= g_arena_base + HMJE_ANDROID_ARENA_SIZE) {
        pthread_mutex_unlock(&g_arena_lock);
        return;
    }
    struct android_heap_block *target = ((struct android_heap_block *)pointer) - 1;
    struct android_heap_block *block = g_arena_blocks;
    struct android_heap_block *previous = NULL;
    while (block != NULL && block != target) {
        previous = block;
        block = block->next;
    }
    if (block == NULL || block->free) {
        pthread_mutex_unlock(&g_arena_lock);
        return;
    }
    block->free = 1;
    if (block->next != NULL && block->next->free) {
        block->size += sizeof(*block) + block->next->size;
        block->next = block->next->next;
    }
    if (previous != NULL && previous->free) {
        previous->size += sizeof(*previous) + block->size;
        previous->next = block->next;
    }
    pthread_mutex_unlock(&g_arena_lock);
}

static i64 vm_release(u64 addr) {
    struct vm_reservation **link = &vm_reservations;
    while (*link != NULL) {
        struct vm_reservation *node = *link;
        if ((u64)(uintptr_t)node->addr == addr) {
            *link = node->next;
            munmap(node->addr, node->size);
            free(node);
            return 0;
        }
        link = &node->next;
    }
    return HMJE_ANDROID_ENOSYS;
}

/* ---- SYS_OBJECT_OPEN_GRAPH_WALK: "/system/fonts/{name}" 프리픽스만 인정 ---- */

static i64 object_open_graph_walk(i64 path_addr, i64 path_len) {
    static const char prefix[] = "/system/fonts/";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (path_len <= 0 || (u64)path_len <= prefix_len) {
        return HMJE_ANDROID_ENOSYS;
    }
    const char *path_bytes = (const char *)(uintptr_t)(u64)path_addr;
    if (memcmp(path_bytes, prefix, prefix_len) != 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    size_t rel_len = (size_t)path_len - prefix_len;
    char rel[256];
    if (rel_len == 0 || rel_len >= sizeof(rel)) {
        return HMJE_ANDROID_ENOSYS;
    }
    memcpy(rel, path_bytes + prefix_len, rel_len);
    rel[rel_len] = '\0';
    if (strstr(rel, "..") != NULL) {
        return HMJE_ANDROID_ENOSYS;
    }
    return hmje_android_open_font_asset(rel, rel_len);
}

/* ---- SYS_SOCKET_CONNECT/SEND/RECV: Bionic TCP client ------------------------------ */

enum android_socket_connect_failure {
    ANDROID_SOCKET_CONNECT_OTHER = 0,
    ANDROID_SOCKET_CONNECT_REFUSED,
    ANDROID_SOCKET_CONNECT_TIMEOUT,
};

static i64 android_socket_connect(i64 host_addr, i64 host_len, i64 port, i64 timeout_ms) {
    if (host_addr == 0 || host_len <= 0 || host_len >= 256 || port <= 0 || port > 65535) {
        return HMJE_ANDROID_ENOSYS;
    }
    const char *host_bytes = (const char *)(uintptr_t)(u64)host_addr;
    if (memchr(host_bytes, '\0', (size_t)host_len) != NULL) {
        return HMJE_ANDROID_ENOSYS;
    }
    char host[256];
    memcpy(host, host_bytes, (size_t)host_len);
    host[host_len] = '\0';
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%lld", (long long)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0 || addresses == NULL) {
        return -(i64)ERR_SOCKET_HOST_NOT_FOUND;
    }

    int effective_timeout = (int)(timeout_ms > 0 ? timeout_ms : SOCKET_DEFAULT_TIMEOUT_MS);
    enum android_socket_connect_failure last_failure = ANDROID_SOCKET_CONNECT_OTHER;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        int original_flags = fcntl(fd, F_GETFL, 0);
        if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
            close(fd);
            continue;
        }
        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result != 0 && errno != EINPROGRESS) {
            last_failure = errno == ECONNREFUSED
                ? ANDROID_SOCKET_CONNECT_REFUSED
                : ANDROID_SOCKET_CONNECT_OTHER;
            close(fd);
            continue;
        }
        if (result != 0) {
            struct pollfd poll_socket;
            poll_socket.fd = fd;
            poll_socket.events = POLLOUT;
            poll_socket.revents = 0;
            int selected = poll(&poll_socket, 1, effective_timeout);
            if (selected == 0) {
                last_failure = ANDROID_SOCKET_CONNECT_TIMEOUT;
                close(fd);
                continue;
            }
            if (selected < 0) {
                last_failure = ANDROID_SOCKET_CONNECT_OTHER;
                close(fd);
                continue;
            }
            int socket_error = 0;
            socklen_t socket_error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_length) != 0 ||
                socket_error != 0) {
                last_failure = socket_error == ECONNREFUSED
                    ? ANDROID_SOCKET_CONNECT_REFUSED
                    : ANDROID_SOCKET_CONNECT_OTHER;
                close(fd);
                continue;
            }
        }
        if (fcntl(fd, F_SETFL, original_flags) != 0) {
            close(fd);
            continue;
        }
        freeaddrinfo(addresses);
        return (i64)fd;
    }

    freeaddrinfo(addresses);
    if (last_failure == ANDROID_SOCKET_CONNECT_REFUSED) {
        return -(i64)ERR_SOCKET_CONN_REFUSED;
    }
    if (last_failure == ANDROID_SOCKET_CONNECT_TIMEOUT) {
        return -(i64)ERR_SOCKET_TIMEOUT;
    }
    return HMJE_ANDROID_ENOSYS;
}

static i64 android_socket_send(i64 handle, i64 in_addr, i64 length) {
    if (handle <= 0 || length < 0 || (length != 0 && in_addr == 0)) {
        return HMJE_ANDROID_ENOSYS;
    }
    if (length == 0) return 0;
    ssize_t written = send(
        (int)handle, (const void *)(uintptr_t)(u64)in_addr, (size_t)length, MSG_NOSIGNAL
    );
    if (written < 0) {
        if (errno == ENOTCONN || errno == EPIPE || errno == ECONNRESET) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_ANDROID_ENOSYS;
    }
    return (i64)written;
}

static i64 android_socket_recv(i64 handle, i64 out_addr, i64 length, i64 flags) {
    if (handle <= 0 || length < 0 || (length != 0 && out_addr == 0) ||
        (flags & ~SOCKET_RECV_NONBLOCK) != 0) {
        return HMJE_ANDROID_ENOSYS;
    }
    if (length == 0) return 0;
    int recv_flags = (flags & SOCKET_RECV_NONBLOCK) != 0 ? MSG_DONTWAIT : 0;
    ssize_t received = recv(
        (int)handle, (void *)(uintptr_t)(u64)out_addr, (size_t)length, recv_flags
    );
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -(i64)ERR_SOCKET_WOULD_BLOCK;
        }
        if (errno == ENOTCONN || errno == ECONNRESET) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_ANDROID_ENOSYS;
    }
    return (i64)received;
}

/* ---- 듣기(서버) 소켓·비차단 송신·비동기 준비 대기: POSIX 백엔드 공유 ---------------------- */
#define HMJE_SOCKET_SERVER_ENOSYS HMJE_ANDROID_ENOSYS
#include "posix_socket_server.inc"

/* 디버그 훅(weak, 기본 no-op): android_jni.c가 __android_log_print로 덮어써서 SYS_WRITE로
 * 나가는 바이트를 logcat에서도 보이게 한다(hmje 자체 stdout은 앱 프로세스에선 보통
 * logcat에 안 잡혀서 디버깅이 어려웠다) — android-exe(순수 shell 실행)는 이 훅이 필요
 * 없고(진짜 stdout이 터미널로 그대로 나간다) 기본 no-op을 그대로 쓴다. */
__attribute__((weak)) void hmje_android_debug_write(const char *bytes, size_t len) {
    (void)bytes;
    (void)len;
}
__attribute__((weak)) void hmje_android_debug_alloc(u64 size, void *result) {
    (void)size;
    (void)result;
}
__attribute__((weak)) i64 hmje_android_gpu_command(i64 kind, i64 request_address, i64 request_length) {
    (void)kind;
    (void)request_address;
    (void)request_length;
    return HMJE_ANDROID_ENOSYS;
}

static i64 android_env_read(i64 name_addr, i64 name_len, i64 out_addr, i64 capacity) {
    if (name_addr == 0 || name_len <= 0 || name_len > 4096) {
        return HMJE_ANDROID_ENOSYS;
    }
    char name[4097];
    memcpy(name, (const void *)(uintptr_t)(u64)name_addr, (size_t)name_len);
    name[name_len] = '\0';
    const char *value = getenv(name);
    if (value == NULL) {
        return -(i64)ERR_ENV_NOT_FOUND;
    }
    size_t len = strlen(value);
    if (out_addr != 0 && capacity >= 0 && (u64)capacity >= len) {
        memcpy((void *)(uintptr_t)(u64)out_addr, value, len);
    }
    return (i64)len;
}

/* ---- 공통 디스패치 ------------------------------------------------------- */

static i64 hmje_posix_dispatch(i64 number, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    switch (number) {
    case SYS_EXIT:
        exit((int)a1);
    case SYS_WRITE: {
        hmje_android_debug_write((const char *)(uintptr_t)(u64)a2, (size_t)a3);
        return (i64)write((int)a1, (const void *)(uintptr_t)(u64)a2, (size_t)a3);
    }
    case SYS_READ:
        return hmje_android_asset_read(a1, a2, a3);
    case SYS_TIME_MS:
        return monotonic_ns() / 1000000LL;
    case SYS_TIME_US:
        return monotonic_ns() / 1000LL;
    case SYS_WALL_TIME_S:
        return wall_time_s();
    case SYS_WALL_TIME_MS:
        return wall_time_ms();
    case SYS_SECURE_RANDOM_FILL:
        return secure_random_fill(a1, a2);
    case SYS_LOCAL_DATETIME:
        if (a2 == 1) return local_datetime_at_seconds(a1);
        if (a2 != 0) return -22;
        return local_datetime_packed();
    case SYS_SLEEP_MS:
        /* 앱 루프가 프레임 사이에 자는 동안 native async 완료가 오면 즉시 깨운다.
         * 완료가 없으면 요청한 시간을 그대로 잔다(비동기 작업이 없는 프로그램은 변화 없음). */
        if (a1 <= 0) return 0;
        (void)hmje_async_sleep_until_wake_ms((uint64_t)a1);
        return 0;
    case SYS_SLEEP_US:
        return sleep_ns(a1 * 1000LL);
    case SYS_ALLOC: {
        i64 block = android_alloc((u64)a1);
        hmje_android_debug_alloc((u64)a1, (void *)(uintptr_t)(u64)block);
        return block;
    }
    case SYS_FREE:
        android_free((u64)a1);
        return 0;
    case SYS_PROCESS_ID:
        return (i64)getpid();
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_VM_RESERVE:
        return vm_reserve((u64)a1);
    case SYS_VM_RELEASE:
        return vm_release((u64)a1);
    case SYS_OBJECT_OPEN_GRAPH_WALK:
        return object_open_graph_walk(a1, a2);
    case SYS_CLOSE:
        return hmje_android_asset_close(a1);
    case SYS_OBJECT_STAT:
        return hmje_android_asset_stat(a1, a2, a3);
    case SYS_OBJECT_READ_AT:
        return hmje_android_asset_read_at(a1, a2, a3, a4);
    case SYS_FRAMEBUFFER_INFO:
        return hmje_android_framebuffer_info(a1, a2);
    case SYS_FRAMEBUFFER_BLIT:
        return hmje_android_framebuffer_blit(a1, a2, a3, a4, a5, a6);
    case SYS_FRAMEBUFFER_PRESENT:
        return hmje_android_framebuffer_present(a1, a2);
    case SYS_INPUT_POLL:
        return hmje_android_input_poll();
    case SYS_INPUT_POLL2:
        return hmje_android_input_poll2(a1, a2);
    case SYS_UI_ACCESSIBILITY:
        return hmje_android_ui_accessibility(a1, a2, a3);
    case SYS_UI_SYSTEM_THEME: {
        const char *override = getenv("HMJE_SYSTEM_THEME");
        if (override != NULL && strcmp(override, "dark") == 0) return 2;
        if (override != NULL && strcmp(override, "light") == 0) return 1;
        return hmje_android_system_theme();
    }
    case SYS_TEXT_INPUT_CONTROL2:
        return hmje_android_text_input_control2(a1, a2, a3);
    case SYS_TEXT_INPUT_CONTROL:
        return hmje_android_text_input_control(a1);
    case SYS_UI_WINDOW_CONFIG:
        return 0; /* Android activity has no desktop title bar; semantic title stays in app metadata. */
    case SYS_UI_WINDOW:
        return a1 == 14 ? hmje_android_app_state() : HMJE_ANDROID_ENOSYS;
    case SYS_UI_CLIPBOARD:
        return HMJE_ANDROID_ENOSYS; /* Android clipboard bridge is a separate follow-up. */
    case SYS_AUDIO_CAPTURE_OPEN:
        return hmje_android_audio_capture_open(a1, a2);
    case SYS_AUDIO_CAPTURE_READ:
        return hmje_android_audio_capture_read(a1, a2, a3, a4, a5);
    case SYS_AUDIO_CAPTURE_CONTROL:
        return hmje_android_audio_capture_control(a1, a2);
    case SYS_AUDIO_CAPTURE_PERMISSION:
        return hmje_android_audio_capture_permission(a1);
    case SYS_SOCKET_CONNECT:
        return android_socket_connect(a1, a2, a3, a4);
    case SYS_SOCKET_SEND:
        return a4 == 0 ? android_socket_send(a1, a2, a3) : hmje_socket_send_flags(a1, a2, a3, a4);
    case SYS_SOCKET_LISTEN:
        return hmje_socket_listen(a1, a2, a3, a4, a5, a6);
    case SYS_SOCKET_ACCEPT:
        return hmje_socket_accept(a1, a2);
    case SYS_SOCKET_RECV:
        return android_socket_recv(a1, a2, a3, a4);
    case SYS_GPU_COMMAND:
        return hmje_android_gpu_command(a1, a2, a3);
    case SYS_ENV_READ:
        return android_env_read(a1, a2, a3, a4);
    default:
        return HMJE_ANDROID_ENOSYS;
    }
}

/* ---- runtime/user/syscall.hmje가 요구하는 4개 심볼 (고정 계약) ------------ */

i64 hmje_arch_syscall0(i64 number) {
    return hmje_posix_dispatch(number, 0, 0, 0, 0, 0, 0);
}

i64 hmje_arch_syscall1(i64 number, i64 arg1) {
    return hmje_posix_dispatch(number, arg1, 0, 0, 0, 0, 0);
}

i64 hmje_arch_syscall3(i64 number, i64 arg1, i64 arg2, i64 arg3) {
    return hmje_posix_dispatch(number, arg1, arg2, arg3, 0, 0, 0);
}

i64 hmje_arch_syscall6(i64 number, i64 arg1, i64 arg2, i64 arg3, i64 arg4, i64 arg5, i64 arg6) {
    return hmje_posix_dispatch(number, arg1, arg2, arg3, arg4, arg5, arg6);
}

/* UTF-8 NFC hook for `문자열NFC정규화`. Paths from this host's pickers are
 * already composed, so the bytes pass through unchanged (macOS normalizes in
 * posix_host_shim.c). output=NULL queries the length. */
int64_t hmje_string_nfc_utf8(const uint8_t *input, uint64_t length,
                             uint8_t *output, uint64_t capacity) {
    if (!input) return -1;
    if (output && capacity >= length) memcpy(output, input, (size_t)length);
    return (int64_t)length;
}
