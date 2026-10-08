/*
 * posix_host_shim.c — hmje 앱을 hosted POSIX 프로세스(macOS/Linux)로 돌릴 때의
 * 시스템콜 구현.
 *
 * semtleOS 위에서는 hmje_arch_syscall0/1/3/6이 CPU 트랩(syscall/svc)으로 커널에
 * 들어가지만(tools/targets/limine.py, tools/targets/aarch64.py), hosted 프로세스에는
 * 그 트랩을 받을 벡터가 없으므로 같은 4개 심볼을 평범한 C 함수로 제공하고, 내부에서
 * SYS_* 번호(runtime/user/syscall.hmje ← kernel/api/syscall.hmje의 고정 계약)를
 * POSIX 호출로 번역한다. 유저/커널 분리가 없으니 copy_from_user 없이 주소를 바로
 * 역참조한다.
 *
 * 프레임버퍼/입력(54-56, 62)은 이 파일에 두지 않는다. macOS는
 * posix_host_window.m(Objective-C, Cocoa), Linux 등 창 없는 hosted POSIX는
 * posix_host_headless.c(화면/GPU만 ENOSYS)를 링크한다. 이 파일은 순수 C로 남겨
 * 어떤 타깃/링크 상황에서도 최소 의존성을 유지한다.
 *
 * 구현된 syscall (docs/syscall.md 의미 준수):
 *   0  SYS_EXIT                  → exit(code)
 *   1  SYS_WRITE                 → write(fd, addr, len)
 *   2  SYS_TIME_MS               → 단조 시계 밀리초 (부팅 이후가 아니라 프로세스 진행시간 기준)
 *   3  SYS_SLEEP_MS              → nanosleep
 *   4  SYS_ALLOC                 → malloc (실패 시 0)
 *   5  SYS_READ                  → read(fd, addr, len) — 객체 핸들도 real fd라 그대로 동작
 *   6  SYS_FREE                  → free, 0 반환
 *   13 SYS_PROCESS_ID            → getpid
 *   15 SYS_YIELD                 → sched_yield, 0 반환
 *   18 SYS_VM_RESERVE            → mmap(anonymous, lazy) (실패 시 0)
 *   19 SYS_VM_RELEASE            → munmap (VM_RESERVE가 기록한 길이로)
 *   20 SYS_OBJECT_OPEN_GRAPH_WALK → "/system/fonts/{name}"은 제품 자원 `fonts/{name}`으로
 *                                   (hmje_resource_open과 같은 자리: 실행파일.assets/…),
 *                                   posix-host 개발 빌드의 "/host/{relative}"는 현재 작업
 *                                   디렉터리의 읽기 전용 일반 파일로 열어 fd를 핸들로 돌려준다.
 *   21 SYS_CLOSE                 → close(fd)
 *   22 SYS_OBJECT_STAT           → fstat(fd)를 64바이트 레코드로 번역(runtime/user/object.hmje
 *                                   의 객체메타데이터가 기대하는 필드만: id/size/format)
 *   37 SYS_OBJECT_READ_AT        → pread(fd, addr, len, offset) — 객체내용읽기(runtime/user/
 *                                   object.hmje)가 쓰는 위치 지정 읽기다(폰트
 *                                   로딩에도 쓰인다). pread는 handle의 streaming offset을
 *                                   안 바꾸는 게 커널이 보장하는 원자적 동작이라 계약과 정확히
 *                                   일치한다.
 *   38 SYS_OBJECT_WRITE_AT       → pwrite(fd, addr, len, offset) — 범용 파일 API(runtime/user/
 *                                   file.hmje)의 파일쓰기가 이 번호를 그대로 쓴다.
 *   42 SYS_OBJECT_SYNC           → macOS F_FULLFSYNC(미지원 시 fsync 폴백), 그 밖의
 *                                   POSIX fsync(fd) — 파일 API와 object API가 같은
 *                                   영속 저장 경계를 공유한다.
 *   54 SYS_FRAMEBUFFER_INFO      → macOS Cocoa 창 / Linux headless ENOSYS
 *   55 SYS_FRAMEBUFFER_BLIT      → macOS Cocoa 창 / Linux headless ENOSYS
 *   56 SYS_INPUT_POLL            → macOS Cocoa 창 / Linux headless no-event
 *   60 SYS_TIME_US               → 단조 시계 마이크로초
 *   61 SYS_SLEEP_US              → nanosleep
 *   62 SYS_FRAMEBUFFER_PRESENT   → posix_host_window.m
 *  117 SYS_UI_WINDOW             → macOS posix_host_window.m(여러 창·창 흐름) /
 *                                   창 없는 hosted POSIX는 ENOSYS
 *   75 SYS_FILE_OPEN             → 범용 파일 API(runtime/user/file.hmje)의 파일열기. 경로를
 *                                   앱 전용 데이터 디렉터리(HMJE_POSIX_FILE_DATA_DIR, 기본
 *                                   "host_app_data/" — CWD 하위) 기준 상대경로로 취급해
 *                                   open()한다. FILE_MODE_CREATE면 mkdir -p로 중간 디렉터리를
 *                                   만들고 O_CREAT로 열며 FILE_MODE_EXCLUSIVE_CREATE도 있으면
 *                                   O_EXCL을 더한다. ".." 성분은 거부한다. macOS .app도 이
 *                                   기본값을 사용한다. 번들 식별자로 Application Support를
 *                                   자동 선택하지 않는다(docs/posix-file-data-root.md).
 *   76 SYS_FILE_LIST             → fdopendir(dup(fd)) + readdir로 디렉터리 자식을
 *                                   FILE_LIST_ENTRY_SIZE(64B) 레코드로 채운다.
 *   91 SYS_FILE_LOCK             → flock(LOCK_SH/LOCK_EX/LOCK_UN) whole-file advisory lock.
 *   92 SYS_FILE_UNLINK           → 같은 자료 루트의 일반 파일을 unlink한 뒤 parent
 *                                   directory를 영속 동기화한다(디렉터리는 제거하지 않음).
 *   94 SYS_FILE_IDENTITY         → 열린 fd의 device/inode 고정 폭 identity.
 *   85 SYS_SOCKET_CONNECT        → 범용 소켓 API(runtime/user/socket.hmje)의 소켓연결.
 *                                   getaddrinfo로 호스트를 풀고(AF_UNSPEC, SOCK_STREAM),
 *                                   각 후보를 논블로킹 connect+poll(타임아웃)로 시도한다.
 *                                   성공하면 fd를 다시 블로킹으로 되돌려 핸들로 돌려준다.
 *                                   실패 사유(호스트 못 찾음/연결 거부/타임아웃)를 각각
 *                                   ERR_SOCKET_*(kernel/api/syscall.hmje)로 구분해 돌려준다.
 *   86 SYS_SOCKET_SEND           → send(fd, addr, len, MSG_NOSIGNAL) — 소켓쓰기.
 *   87 SYS_SOCKET_RECV           → recv(fd, addr, len, 0) — 소켓읽기.
 *  118 SYS_SOCKET_LISTEN         → 루프백 전용(명시 opt-in 없으면) bind+listen — 소켓듣기.
 *  119 SYS_SOCKET_ACCEPT         → accept(비차단 선택) — 소켓수락/소켓수락시도.
 *                                   posix_socket_server.inc(Android와 공유)가 구현한다.
 *   99 SYS_ARG_SIZES             → real argc/argv 크기({개수, 총바이트} 16B 기록)
 *  100 SYS_ARG_READ              → argv를 NUL 종료 연접 블록으로 일괄 복사
 *  101 SYS_ENV_READ              → getenv(이름) 값 복사(없으면 -ERR_ENV_NOT_FOUND)
 *
 * 스텁(HMJE_POSIX_ENOSYS = -38 반환, 크래시하지 않음):
 *   - 객체 그래프 전용 나머지 전부(생성/링크/엣지/이름/format 등, 23-36, 39-45, 51-52, 63) —
 *     열기(20)/닫기(21)/stat(22)/read(공용 5)/read_at(37)/write_at(38) 외엔 미구현.
 *     "/host/"는 posix-host 개발 빌드에서만 켜지고 iOS/Android/semtleOS 계약에는
 *     포함되지 않는다(SYS_FILE_OPEN/LIST와는 별개 — 그쪽은 항상 켜져 있다).
 *   - IPC (SYS_IPC_*, 8-12, 14, 64-65)
 *   - 부트 모듈 (SYS_BOOT_MODULE_*, 57-58, 84)
 *   - 디버그/진단 (7, 16-17, 39, 46-50, 59, 66)
 *   - SYS_VM_SYNC(53)
 *
 * 객체 핸들은 실제 POSIX fd이므로 SYS_READ와 SYS_CLOSE에 그대로 넘길 수 있다.
 * 객체내용읽기는 SYS_OBJECT_READ_AT으로 위치 지정 읽기를 하고, 객체핸들닫기는
 * SYS_CLOSE로 같은 핸들을 닫는다.
 */



#if defined(__linux__) && !defined(_GNU_SOURCE)
/* Linux libc의 POSIX/GNU socket·file API 선언보다 반드시 먼저 와야 한다.
 * 이 translation unit 안에만 한정해 다른 native adapter의 헤더 노출은 바꾸지 않는다. */
#define _GNU_SOURCE 1
#endif

#include <dirent.h>
#include <errno.h>
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <TargetConditionals.h>
#endif

#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif
#include <spawn.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif

/* Only groups launched by the configured pipe/PTY APIs are eligible for cleanup. */
static pthread_mutex_t hmje_child_groups_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t hmje_child_groups[4096];
static int hmje_child_groups_exit_hook;
static void hmje_child_groups_cleanup(void) {
    pthread_mutex_lock(&hmje_child_groups_lock);
    for (size_t i = 0; i < 4096; ++i)
        if (hmje_child_groups[i] > 0)
            (void)kill(-hmje_child_groups[i], SIGTERM);
    pthread_mutex_unlock(&hmje_child_groups_lock);
}
static void hmje_child_group_track(pid_t child) {
    pthread_mutex_lock(&hmje_child_groups_lock);
    if (!hmje_child_groups_exit_hook) {
        (void)atexit(hmje_child_groups_cleanup);
        hmje_child_groups_exit_hook = 1;
    }
    for (size_t i = 0; i < 4096; ++i)
        if (hmje_child_groups[i] == 0) { hmje_child_groups[i] = child; break; }
    pthread_mutex_unlock(&hmje_child_groups_lock);
}
static void hmje_child_group_untrack(pid_t child) {
    pthread_mutex_lock(&hmje_child_groups_lock);
    for (size_t i = 0; i < 4096; ++i)
        if (hmje_child_groups[i] == child) { hmje_child_groups[i] = 0; break; }
    pthread_mutex_unlock(&hmje_child_groups_lock);
}
#if defined(__APPLE__)
#include <dlfcn.h>
#include <sys/sysctl.h>
#endif

/* UTF-8 NFC: query with output=NULL, then write into a caller-owned buffer. */
int64_t hmje_string_nfc_utf8(const uint8_t *input, uint64_t length,
                             uint8_t *output, uint64_t capacity) {
    if (!input || length > LONG_MAX) return -1;
#if defined(__APPLE__)
    CFStringRef original = CFStringCreateWithBytes(kCFAllocatorDefault, input,
        (CFIndex)length, kCFStringEncodingUTF8, false);
    if (!original) return -1;
    CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, original);
    CFRelease(original);
    if (!normalized) return -1;
    CFStringNormalize(normalized, kCFStringNormalizationFormC);
    CFIndex used = 0;
    CFIndex count = CFStringGetBytes(normalized, CFRangeMake(0, CFStringGetLength(normalized)),
        kCFStringEncodingUTF8, 0, false, NULL, 0, &used);
    if (count != CFStringGetLength(normalized)) { CFRelease(normalized); return -1; }
    if (output && capacity >= (uint64_t)used) {
        CFIndex written = 0;
        CFIndex converted = CFStringGetBytes(normalized,
            CFRangeMake(0, CFStringGetLength(normalized)), kCFStringEncodingUTF8,
            0, false, output, (CFIndex)capacity, &written);
        if (converted != CFStringGetLength(normalized) || written != used) {
            CFRelease(normalized); return -1;
        }
    }
    CFRelease(normalized);
    return (int64_t)used;
#else
    if (output && capacity >= length) memcpy(output, input, (size_t)length);
    return (int64_t)length;
#endif
}

/*
 * Hosted compiler/process coordinator boundary. `argument_pairs` points to
 * argc pairs of [byte-address, byte-length]. HMJE strings need not be NUL
 * terminated; this function owns temporary C copies until posix_spawnp has
 * consumed argv. Start returns a positive child PID or -errno. Wait and the
 * synchronous wrapper return a normal 0..255 status, 128+signal, or -errno.
 */
int64_t hmje_posix_process_start(uint64_t argument_pairs, uint64_t argc) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return -ENOSYS;
#else
    if (argument_pairs == 0 || argc == 0 || argc > 4096) return -EINVAL;
    if (argc > (SIZE_MAX / sizeof(char *)) - 1) return -EOVERFLOW;

    const uint64_t *pairs = (const uint64_t *)(uintptr_t)argument_pairs;
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (argv == NULL) return -ENOMEM;

    int error = 0;
    for (uint64_t index = 0; index < argc; ++index) {
        uint64_t address = pairs[index * 2];
        uint64_t length = pairs[index * 2 + 1];
        if (address == 0 || length > SIZE_MAX - 1) {
            error = EINVAL;
            break;
        }
        const void *source = (const void *)(uintptr_t)address;
        if (memchr(source, '\0', (size_t)length) != NULL) {
            error = EINVAL;
            break;
        }
        argv[index] = (char *)malloc((size_t)length + 1);
        if (argv[index] == NULL) {
            error = ENOMEM;
            break;
        }
        memcpy(argv[index], source, (size_t)length);
        argv[index][length] = '\0';
    }

    pid_t child = 0;
    if (error == 0) {
#if defined(__APPLE__)
        char **environment = *_NSGetEnviron();
#else
        extern char **environ;
        char **environment = environ;
#endif
        error = posix_spawnp(&child, argv[0], NULL, NULL, argv, environment);
    }

    for (uint64_t index = 0; index < argc; ++index) free(argv[index]);
    free(argv);
    if (error != 0) return -(int64_t)error;
    return (int64_t)child;
#endif
}

int64_t hmje_posix_process_wait(int64_t child) {
    if (child <= 0 || child > INT32_MAX) return -EINVAL;
    int status = 0;
    while (waitpid((pid_t)child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -(int64_t)errno;
    }
    hmje_child_group_untrack((pid_t)child);
    if (WIFEXITED(status)) return (int64_t)WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return (int64_t)(128 + WTERMSIG(status));
    return -ECHILD;
}

/*
 * Spawn with independently owned stdin/stdout/stderr pipes.  The four signed
 * 64-bit result slots are pid, stdin write fd, stdout read fd, stderr read fd.
 * Parent-side descriptors are non-blocking so an event loop can drain stdout
 * and stderr without allowing either stream to deadlock the other.
 */
static int hmje_process_environment_name_matches(const char *entry, const char *override) {
    const char *entry_equal = strchr(entry, '=');
    const char *override_equal = strchr(override, '=');
    if (entry_equal == NULL || override_equal == NULL || override_equal == override) return 0;
    size_t entry_length = (size_t)(entry_equal - entry);
    size_t override_length = (size_t)(override_equal - override);
    return entry_length == override_length && memcmp(entry, override, entry_length) == 0;
}

int64_t hmje_posix_process_pipe_start_config(
    uint64_t argument_pairs,
    uint64_t argc,
    uint64_t working_directory_address,
    uint64_t working_directory_length,
    uint64_t environment_pairs,
    uint64_t environment_count,
    uint64_t result_address
) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return -ENOSYS;
#else
    if (argument_pairs == 0 || argc == 0 || argc > 4096 || result_address == 0 ||
        working_directory_length > SIZE_MAX - 1 || environment_count > 4096 ||
        (working_directory_length > 0 && working_directory_address == 0) ||
        (environment_count > 0 && environment_pairs == 0)) {
        return -EINVAL;
    }
    if (argc > (SIZE_MAX / sizeof(char *)) - 1) return -EOVERFLOW;

    const uint64_t *pairs = (const uint64_t *)(uintptr_t)argument_pairs;
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (argv == NULL) return -ENOMEM;
    int error = 0;
    for (uint64_t index = 0; index < argc; ++index) {
        uint64_t address = pairs[index * 2];
        uint64_t length = pairs[index * 2 + 1];
        if (address == 0 || length > SIZE_MAX - 1 ||
            memchr((const void *)(uintptr_t)address, '\0', (size_t)length) != NULL) {
            error = EINVAL;
            break;
        }
        argv[index] = (char *)malloc((size_t)length + 1);
        if (argv[index] == NULL) {
            error = ENOMEM;
            break;
        }
        memcpy(argv[index], (const void *)(uintptr_t)address, (size_t)length);
        argv[index][length] = '\0';
    }

    char *working_directory = NULL;
    if (error == 0 && working_directory_length > 0) {
        working_directory = (char *)malloc((size_t)working_directory_length + 1);
        if (working_directory == NULL) error = ENOMEM;
        else {
            memcpy(working_directory, (const void *)(uintptr_t)working_directory_address,
                   (size_t)working_directory_length);
            working_directory[working_directory_length] = '\0';
        }
    }

    const uint64_t *environment_input = (const uint64_t *)(uintptr_t)environment_pairs;
    char **overrides = NULL;
    if (error == 0 && environment_count > 0) {
        overrides = (char **)calloc((size_t)environment_count, sizeof(char *));
        if (overrides == NULL) error = ENOMEM;
    }
    for (uint64_t index = 0; error == 0 && index < environment_count; ++index) {
        uint64_t address = environment_input[index * 2];
        uint64_t length = environment_input[index * 2 + 1];
        if (address == 0 || length > SIZE_MAX - 1 ||
            memchr((const void *)(uintptr_t)address, '\0', (size_t)length) != NULL) {
            error = EINVAL;
            break;
        }
        overrides[index] = (char *)malloc((size_t)length + 1);
        if (overrides[index] == NULL) { error = ENOMEM; break; }
        memcpy(overrides[index], (const void *)(uintptr_t)address, (size_t)length);
        overrides[index][length] = '\0';
        char *equal = strchr(overrides[index], '=');
        if (equal == NULL || equal == overrides[index]) error = EINVAL;
    }

#if defined(__APPLE__)
    char **inherited_environment = *_NSGetEnviron();
#else
    extern char **environ;
    char **inherited_environment = environ;
#endif
    size_t inherited_count = 0;
    while (inherited_environment[inherited_count] != NULL) inherited_count++;
    char **child_environment = NULL;
    if (error == 0) {
        child_environment = (char **)calloc(inherited_count + (size_t)environment_count + 1,
                                             sizeof(char *));
        if (child_environment == NULL) error = ENOMEM;
    }
    size_t child_environment_count = 0;
    for (size_t index = 0; error == 0 && index < inherited_count; ++index) {
        int replaced = 0;
        for (uint64_t override_index = 0; override_index < environment_count; ++override_index) {
            if (hmje_process_environment_name_matches(inherited_environment[index], overrides[override_index])) {
                replaced = 1;
                break;
            }
        }
        if (!replaced) child_environment[child_environment_count++] = inherited_environment[index];
    }
    for (uint64_t index = 0; error == 0 && index < environment_count; ++index)
        child_environment[child_environment_count++] = overrides[index];

    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    int actions_ready = 0;
    posix_spawnattr_t attributes;
    int attributes_ready = 0;
    pid_t child = 0;
    if (error == 0 && pipe(input_pipe) != 0) error = errno;
    if (error == 0 && pipe(output_pipe) != 0) error = errno;
    if (error == 0 && pipe(error_pipe) != 0) error = errno;
    if (error == 0) {
        error = posix_spawn_file_actions_init(&actions);
        if (error == 0) actions_ready = 1;
    }
    if (error == 0) {
        error = posix_spawnattr_init(&attributes);
        if (error == 0) attributes_ready = 1;
    }
    if (error == 0) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (error == 0) error = posix_spawnattr_setpgroup(&attributes, 0);
#define HMJE_SPAWN_ACTION(call) do { if (error == 0) error = (call); } while (0)
    if (error == 0) {
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, input_pipe[0]));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, input_pipe[1]));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, output_pipe[0]));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, output_pipe[1]));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, error_pipe[0]));
        HMJE_SPAWN_ACTION(posix_spawn_file_actions_addclose(&actions, error_pipe[1]));
        if (working_directory != NULL) {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
            /* iOS/iPadOS explicitly marks both addchdir variants unavailable.
             * Process launch is not an app-sandbox capability; reject this
             * optional configuration without making every iOS app fail to
             * compile against the shared POSIX shim. */
            error = ENOSYS;
#else
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
            HMJE_SPAWN_ACTION(posix_spawn_file_actions_addchdir(&actions, working_directory));
#else
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            HMJE_SPAWN_ACTION(posix_spawn_file_actions_addchdir_np(&actions, working_directory));
#pragma clang diagnostic pop
#endif
#endif
#else
            HMJE_SPAWN_ACTION(posix_spawn_file_actions_addchdir_np(&actions, working_directory));
#endif
        }
    }
#undef HMJE_SPAWN_ACTION
    if (error == 0) {
        error = posix_spawnp(&child, argv[0], &actions, &attributes, argv, child_environment);
    }
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    for (uint64_t index = 0; index < argc; ++index) free(argv[index]);
    free(argv);
    free(working_directory);
    if (overrides != NULL) {
        for (uint64_t index = 0; index < environment_count; ++index) free(overrides[index]);
    }
    free(overrides);
    free(child_environment);

    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (output_pipe[1] >= 0) close(output_pipe[1]);
    if (error_pipe[1] >= 0) close(error_pipe[1]);
    if (error != 0) {
        if (input_pipe[1] >= 0) close(input_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (error_pipe[0] >= 0) close(error_pipe[0]);
        return -(int64_t)error;
    }

    int flags = fcntl(input_pipe[1], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(input_pipe[1], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(error_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(error_pipe[0], F_SETFL, flags | O_NONBLOCK);
    int64_t *result = (int64_t *)(uintptr_t)result_address;
    result[0] = (int64_t)child;
    result[1] = (int64_t)input_pipe[1];
    result[2] = (int64_t)output_pipe[0];
    result[3] = (int64_t)error_pipe[0];
    hmje_child_group_track(child);
    return 0;
#endif
}

int64_t hmje_posix_process_pipe_start(
    uint64_t argument_pairs,
    uint64_t argc,
    uint64_t result_address
) {
    return hmje_posix_process_pipe_start_config(
        argument_pairs, argc, 0, 0, 0, 0, result_address);
}

int64_t hmje_posix_process_pipe_read(int64_t descriptor, uint64_t address, uint64_t capacity) {
    if (descriptor < 0 || address == 0 || capacity == 0 || capacity > (uint64_t)SSIZE_MAX) return -EINVAL;
    ssize_t count;
    do {
        count = read((int)descriptor, (void *)(uintptr_t)address, (size_t)capacity);
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -11;
    return count < 0 ? -(int64_t)errno : (int64_t)count;
}

int64_t hmje_posix_process_pipe_write(int64_t descriptor, uint64_t address, uint64_t length) {
    if (descriptor < 0 || (address == 0 && length != 0) || length > (uint64_t)SSIZE_MAX) return -EINVAL;
    ssize_t count;
    do {
        count = write((int)descriptor, (const void *)(uintptr_t)address, (size_t)length);
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -11;
    return count < 0 ? -(int64_t)errno : (int64_t)count;
}

int64_t hmje_posix_process_pipe_close(int64_t descriptor) {
    if (descriptor < 0 || descriptor > INT32_MAX) return -EINVAL;
    return close((int)descriptor) == 0 ? 0 : -(int64_t)errno;
}

int64_t hmje_posix_process_terminate(int64_t child) {
    if (child <= 0 || child > INT32_MAX) return -EINVAL;
    return kill((pid_t)child, SIGTERM) == 0 ? 0 : -(int64_t)errno;
}

int64_t hmje_host_process_pipe_start_config(
    uint64_t argument_pairs, uint64_t argc,
    uint64_t working_directory_address, uint64_t working_directory_length,
    uint64_t environment_pairs, uint64_t environment_count,
    uint64_t result_address
) {
    return hmje_posix_process_pipe_start_config(argument_pairs, argc,
        working_directory_address, working_directory_length,
        environment_pairs, environment_count, result_address);
}

int64_t hmje_host_process_pipe_start(uint64_t argument_pairs, uint64_t argc,
                                     uint64_t result_address) {
    return hmje_posix_process_pipe_start(argument_pairs, argc, result_address);
}

int64_t hmje_host_process_pipe_read(int64_t descriptor, uint64_t address,
                                    uint64_t capacity) {
    return hmje_posix_process_pipe_read(descriptor, address, capacity);
}

int64_t hmje_host_process_pipe_write(int64_t descriptor, uint64_t address,
                                     uint64_t length) {
    return hmje_posix_process_pipe_write(descriptor, address, length);
}

int64_t hmje_host_process_pipe_close(int64_t descriptor) {
    return hmje_posix_process_pipe_close(descriptor);
}

int64_t hmje_host_process_terminate(int64_t child) {
    return hmje_posix_process_terminate(child);
}

/* status[0]: 0 running, 1 exited, 2 signal; status[1]: code or signal. */
int64_t hmje_host_process_status(int64_t child, uint64_t result_address) {
    if (child <= 0 || child > INT32_MAX || result_address == 0) return -EINVAL;
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int waited;
    do { waited = waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT); }
    while (waited < 0 && errno == EINTR);
    if (waited < 0) return -(int64_t)errno;
    int64_t *result = (int64_t *)(uintptr_t)result_address;
    result[0] = info.si_pid == 0 ? 0 : info.si_code == CLD_EXITED ? 1 :
        (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) ? 2 : 3;
    result[1] = info.si_pid == 0 ? 0 : info.si_status;
    return 0;
}

int64_t hmje_host_process_group_signal(int64_t child, int64_t signal_number) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return -ENOSYS;
#else
    if (child <= 0 || child > INT32_MAX || signal_number <= 0 || signal_number >= NSIG)
        return -EINVAL;
    /* Never address the caller's or an unrelated process group. */
    pthread_mutex_lock(&hmje_child_groups_lock);
    int found = 0;
    for (size_t i = 0; i < 4096; ++i)
        if (hmje_child_groups[i] == (pid_t)child) { found = 1; break; }
    pthread_mutex_unlock(&hmje_child_groups_lock);
    if (!found) return -ESRCH;
    return kill(-(pid_t)child, (int)signal_number) == 0 ? 0 : -(int64_t)errno;
#endif
}

int64_t hmje_host_process_signal(int64_t child, int64_t signal_number) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return -ENOSYS;
#else
    if (child <= 0 || child > INT32_MAX || signal_number <= 0 || signal_number >= NSIG)
        return -EINVAL;
    pthread_mutex_lock(&hmje_child_groups_lock);
    int found = 0;
    for (size_t i = 0; i < 4096; ++i)
        if (hmje_child_groups[i] == (pid_t)child) { found = 1; break; }
    pthread_mutex_unlock(&hmje_child_groups_lock);
    if (!found) return -ESRCH;
    return kill((pid_t)child, (int)signal_number) == 0 ? 0 : -(int64_t)errno;
#endif
}

#include "posix_host_pty.inc"

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
typedef struct { pid_t child; int fd; uint64_t token; } HmjeProcessReadyRequest;
static void *hmje_process_ready_worker(void *pointer) {
    HmjeProcessReadyRequest *request = pointer;
    int error = 0;
    uint64_t result = 0;
#if defined(__APPLE__)
    int queue = kqueue();
    if (queue < 0) error = errno;
    if (!error) {
        struct kevent changes[2];
        int count = 0;
        if (request->fd >= 0) EV_SET(&changes[count++], request->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
        EV_SET(&changes[count++], request->child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        struct kevent event;
        if (kevent(queue, changes, count, &event, 1, NULL) < 0) error = errno;
        else if (event.flags & EV_ERROR) error = (int)event.data;
        else result = event.filter == EVFILT_PROC ? 2 : 1;
        close(queue);
    }
#else
    int process_fd = -1;
#if defined(SYS_pidfd_open)
    process_fd = (int)syscall(SYS_pidfd_open, request->child, 0);
#endif
    if (process_fd < 0 && request->fd >= 0) {
        error = ENOSYS;
    } else if (process_fd < 0) {
        /* A dedicated worker may block in waitpid when pidfd is unavailable.
         * This only applies to a wait with no pipe/PTY descriptor. */
        int status = 0;
        if (waitpid(request->child, &status, 0) < 0) error = errno;
        else { hmje_child_group_untrack(request->child); result = 2; }
    } else {
        struct pollfd watched[2];
        int count = 0;
        if (request->fd >= 0) watched[count++] = (struct pollfd){request->fd, POLLIN | POLLHUP, 0};
        if (process_fd >= 0) watched[count++] = (struct pollfd){process_fd, POLLIN, 0};
        int ready;
        do { ready = poll(watched, count, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) error = errno;
        else result = process_fd >= 0 && (watched[count - 1].revents & POLLIN) ? 2 : 1;
    }
    if (process_fd >= 0) close(process_fd);
#endif
    hmje_async_complete_u64(request->token, error, result);
    free(request);
    return NULL;
}
void hmje_host_process_wait_io_async(int64_t child, int64_t descriptor, uint64_t token) {
    if (child <= 0 || child > INT32_MAX || descriptor > INT32_MAX || descriptor < -1) {
        hmje_async_complete_u64(token, EINVAL, 0); return;
    }
    HmjeProcessReadyRequest *request = malloc(sizeof(*request));
    if (!request) { hmje_async_complete_u64(token, ENOMEM, 0); return; }
    *request = (HmjeProcessReadyRequest){(pid_t)child, (int)descriptor, token};
    pthread_t worker;
    int error = pthread_create(&worker, NULL, hmje_process_ready_worker, request);
    if (error) { free(request); hmje_async_complete_u64(token, error, 0); }
    else pthread_detach(worker);
}

int64_t hmje_posix_process_run(uint64_t argument_pairs, uint64_t argc) {
    int64_t child = hmje_posix_process_start(argument_pairs, argc);
    return child < 0 ? child : hmje_posix_process_wait(child);
}

int64_t hmje_host_process_start(uint64_t argument_pairs, uint64_t argc) {
    return hmje_posix_process_start(argument_pairs, argc);
}

int64_t hmje_host_process_wait(int64_t child) {
    return hmje_posix_process_wait(child);
}

int64_t hmje_host_process_run(uint64_t argument_pairs, uint64_t argc) {
    return hmje_posix_process_run(argument_pairs, argc);
}

int64_t hmje_posix_directory_create_all(uint64_t path_address, uint64_t path_length) {
    if (path_address == 0 || path_length == 0 || path_length >= 65536) return -EINVAL;
    const void *source = (const void *)(uintptr_t)path_address;
    if (memchr(source, '\0', (size_t)path_length) != NULL) return -EINVAL;
    char *path = (char *)malloc((size_t)path_length + 1);
    if (path == NULL) return -ENOMEM;
    memcpy(path, source, (size_t)path_length);
    path[path_length] = '\0';

    for (uint64_t index = 1; index <= path_length; ++index) {
        if (index != path_length && path[index] != '/') continue;
        char saved = path[index];
        path[index] = '\0';
        if (path[0] != '\0' && mkdir(path, 0777) != 0 && errno != EEXIST) {
            int error = errno;
            free(path);
            return -(int64_t)error;
        }
        path[index] = saved;
    }
    free(path);
    return 0;
}

int64_t hmje_host_directory_create_all(uint64_t path_address, uint64_t path_length) {
    return hmje_posix_directory_create_all(path_address, path_length);
}

/*
 * 선택적 hosted 진단 allocator. 기본 실행 경로는 그대로 malloc/free를 쓰며,
 * HMJE_TRACK_ALLOCATIONS가 0이 아닌 경우에만 header 검증과 bounded quarantine을
 * 켠다. HMJE_TRACK_QUARANTINE_BYTES로 quarantine 상한을 정할 수 있다.
 * HMJE_FAIL_ALLOC_AFTER=N은 테스트에서 앞의 N번만 성공시키고 이후 SYS_ALLOC을
 * 0으로 돌려준다. 기본값은 완전 비활성이며 native 라이브러리 malloc은 건드리지
 * 않는다. 실패 여부를 tracking header 할당보다 먼저 결정하므로 live/quarantine
 * 회계에는 성공한 HMJE 블록만 들어간다.
 *
 * 이 경계는 SQLite 같은 native 라이브러리의 할당을 가로채지 않고 SYS_ALLOC/
 * SYS_FREE로 오간 HMJE 소유 블록만 추적한다. 따라서 owner lowering의 조기 해제,
 * 이중 해제와 일부 use-after-free를 실제 hosted 서비스 조합에서 재현할 수 있다.
 */
typedef struct hmje_tracked_allocation {
    uint64_t magic;
    uint64_t size;
    struct hmje_tracked_allocation *next;
    struct hmje_tracked_allocation *previous;
    void *diagnostic_trace;
} hmje_tracked_allocation;

#if defined(__APPLE__) || defined(__linux__)
typedef struct hmje_allocation_trace {
    int frame_count;
    void *frames[32];
} hmje_allocation_trace;
#endif

#define HMJE_TRACKED_LIVE 0x484d4a454c495645ULL
#define HMJE_TRACKED_FREED 0x484d4a4546524545ULL
#define HMJE_DEFAULT_QUARANTINE_BYTES (64ULL * 1024ULL * 1024ULL)

static int hmje_track_allocations = -1;
/* 추적 목록·계수는 스레드가 여럿이면 잠가야 한다. 추적이 꺼진 경로(기본)는
 * 이 잠금을 만지지 않으므로 비용이 없다. */

static pthread_mutex_t hmje_track_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t hmje_live_allocations = 0;
static uint64_t hmje_quarantine_bytes = 0;
static uint64_t hmje_quarantine_limit = HMJE_DEFAULT_QUARANTINE_BYTES;
static hmje_tracked_allocation *hmje_live_head = NULL;
static hmje_tracked_allocation *hmje_quarantine_head = NULL;
static hmje_tracked_allocation *hmje_quarantine_tail = NULL;
static int hmje_fail_alloc_enabled = 0;
static uint64_t hmje_fail_alloc_after = 0;
static uint64_t hmje_allocation_attempts = 0;

/* SYS_ALLOC can now arrive from several HMJE threads at once: the standard
 * library skips its global allocator lock while its size-class cache is off.
 * Environment-derived settings are therefore read exactly once, and the
 * fail-after counter is claimed atomically so N successes stay exact. */
static pthread_once_t hmje_fail_alloc_once = PTHREAD_ONCE_INIT;

static void hmje_fail_alloc_read_environment(void) {
    const char *value = getenv("HMJE_FAIL_ALLOC_AFTER");
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0') {
            hmje_fail_alloc_after = (uint64_t)parsed;
            __atomic_store_n(&hmje_fail_alloc_enabled, 1, __ATOMIC_RELEASE);
        }
    }
}

/* Test-only controls: arm relative to the next SYS_ALLOC without depending on
 * allocations performed during process/bootstrap setup. They are deliberately
 * not part of the HMJE syscall ABI, and are called before worker threads. */
void hmje_test_fail_alloc_after_from_now(uint64_t successful_allocations) {
    pthread_once(&hmje_fail_alloc_once, hmje_fail_alloc_read_environment);
    hmje_fail_alloc_after = successful_allocations;
    __atomic_store_n(&hmje_allocation_attempts, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&hmje_fail_alloc_enabled, 1, __ATOMIC_RELEASE);
}

void hmje_test_fail_alloc_disable(void) {
    pthread_once(&hmje_fail_alloc_once, hmje_fail_alloc_read_environment);
    __atomic_store_n(&hmje_fail_alloc_enabled, 0, __ATOMIC_RELEASE);
    hmje_fail_alloc_after = 0;
    __atomic_store_n(&hmje_allocation_attempts, 0, __ATOMIC_RELAXED);
}

static _Thread_local int hmje_worker_allocator_active;

static int hmje_should_fail_allocation(void) {
    /* Callback workers initialize both policies once on entry. Keep the
     * atomic test control live, but avoid pthread_once on every tiny block. */
    if (!hmje_worker_allocator_active)
        pthread_once(&hmje_fail_alloc_once, hmje_fail_alloc_read_environment);
    if (!__atomic_load_n(&hmje_fail_alloc_enabled, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    uint64_t attempt = __atomic_load_n(&hmje_allocation_attempts, __ATOMIC_RELAXED);
    while (attempt < hmje_fail_alloc_after) {
        if (__atomic_compare_exchange_n(&hmje_allocation_attempts, &attempt, attempt + 1,
                                        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    return 1;
}

static int hmje_trace_allocation_size(size_t size) {
    const char *value = getenv("HMJE_TRACE_ALLOC_SIZE");
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' &&
           parsed == (unsigned long long)size;
}

static pthread_once_t hmje_track_allocations_once = PTHREAD_ONCE_INIT;

static void hmje_track_allocations_read_environment(void) {
    const char *enabled = getenv("HMJE_TRACK_ALLOCATIONS");
    const char *limit = getenv("HMJE_TRACK_QUARANTINE_BYTES");
    int tracking = enabled != NULL && enabled[0] != '\0' && strcmp(enabled, "0") != 0;
    if (tracking && limit != NULL && limit[0] != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(limit, &end, 10);
        if (errno == 0 && end != limit && *end == '\0') {
            hmje_quarantine_limit = (uint64_t)parsed;
        }
    }
    hmje_track_allocations = tracking;
}

static int hmje_allocation_tracking_enabled(void) {
    if (!hmje_worker_allocator_active)
        pthread_once(&hmje_track_allocations_once, hmje_track_allocations_read_environment);
    return hmje_track_allocations;
}

static void hmje_quarantine_trim(void) {
    while (hmje_quarantine_head != NULL &&
           hmje_quarantine_bytes > hmje_quarantine_limit) {
        hmje_tracked_allocation *released = hmje_quarantine_head;
        hmje_quarantine_head = released->next;
        if (hmje_quarantine_head == NULL) {
            hmje_quarantine_tail = NULL;
        }
        hmje_quarantine_bytes -= released->size;
        free(released);
    }
}

/* Compiler callbacks are short-lived and allocate many small HMJE values.
 * Keep freed malloc blocks on the callback's own thread only. The cache is
 * drained before that worker exits; a value passed to the joining thread is
 * still an ordinary malloc allocation and can be freed there. Allocation
 * tracking and fault injection retain their original paths. */
#define HMJE_WORKER_CACHE_BINS 10
#define HMJE_WORKER_CACHE_LIMIT 512
static _Thread_local struct {
    void *heads[HMJE_WORKER_CACHE_BINS];
    size_t bytes;
    unsigned count;
    int active;
} hmje_worker_alloc_cache;

static size_t hmje_worker_usable_size(void *pointer) {
#if defined(__APPLE__)
    return malloc_size(pointer);
#elif defined(__linux__)
    return malloc_usable_size(pointer);
#else
    (void)pointer;
    return 0;
#endif
}

static unsigned hmje_worker_cache_bin(size_t size) {
    size_t bound = 32;
    for (unsigned bin = 0; bin < HMJE_WORKER_CACHE_BINS; ++bin, bound <<= 1)
        if (size <= bound) return bin;
    return HMJE_WORKER_CACHE_BINS;
}

void hmje_posix_allocator_worker_enter(void) {
    pthread_once(&hmje_fail_alloc_once, hmje_fail_alloc_read_environment);
    pthread_once(&hmje_track_allocations_once, hmje_track_allocations_read_environment);
    hmje_worker_alloc_cache.active = 1;
    hmje_worker_allocator_active = 1;
}

void hmje_posix_allocator_worker_leave(void) {
    hmje_worker_alloc_cache.active = 0;
    hmje_worker_allocator_active = 0;
    for (unsigned bin = 0; bin < HMJE_WORKER_CACHE_BINS; ++bin) {
        void *block = hmje_worker_alloc_cache.heads[bin];
        hmje_worker_alloc_cache.heads[bin] = NULL;
        while (block) {
            void *next = *(void **)block;
            free(block);
            block = next;
        }
    }
    hmje_worker_alloc_cache.bytes = 0;
    hmje_worker_alloc_cache.count = 0;
}

static void *hmje_host_allocate(size_t size) {
    if (hmje_should_fail_allocation()) {
        return NULL;
    }
    if (!hmje_allocation_tracking_enabled()) {
        if (hmje_worker_alloc_cache.active) {
            for (unsigned bin = hmje_worker_cache_bin(size);
                 bin < HMJE_WORKER_CACHE_BINS; ++bin) {
                void **slot = &hmje_worker_alloc_cache.heads[bin];
                while (*slot) {
                    void *block = *slot;
                    size_t capacity = hmje_worker_usable_size(block);
                    if (capacity >= size) {
                        *slot = *(void **)block;
                        hmje_worker_alloc_cache.bytes -= capacity;
                        --hmje_worker_alloc_cache.count;
                        return block;
                    }
                    slot = (void **)block;
                }
            }
        }
        return malloc(size);
    }
    if (size > SIZE_MAX - sizeof(hmje_tracked_allocation)) {
        return NULL;
    }
    hmje_tracked_allocation *allocation =
        malloc(sizeof(hmje_tracked_allocation) + size);
    if (allocation == NULL) {
        return NULL;
    }
    allocation->magic = HMJE_TRACKED_LIVE;
    allocation->size = (uint64_t)size;
    allocation->next = NULL;
    allocation->previous = NULL;
    allocation->diagnostic_trace = NULL;
#if defined(__APPLE__) || defined(__linux__)
    if (hmje_trace_allocation_size(size)) {
        hmje_allocation_trace *trace = malloc(sizeof(*trace));
        if (trace != NULL) {
            trace->frame_count = backtrace(
                trace->frames,
                (int)(sizeof(trace->frames) / sizeof(trace->frames[0]))
            );
            allocation->diagnostic_trace = trace;
        }
    }
#endif
    pthread_mutex_lock(&hmje_track_lock);
    allocation->next = hmje_live_head;
    if (hmje_live_head != NULL) {
        hmje_live_head->previous = allocation;
    }
    hmje_live_head = allocation;
    hmje_live_allocations += 1;
    pthread_mutex_unlock(&hmje_track_lock);
    return allocation + 1;
}

static void hmje_host_release(void *pointer) {
    if (!hmje_allocation_tracking_enabled()) {
        if (pointer && hmje_worker_alloc_cache.active) {
            size_t capacity = hmje_worker_usable_size(pointer);
            unsigned bin = hmje_worker_cache_bin(capacity);
            if (capacity >= sizeof(void *) && bin < HMJE_WORKER_CACHE_BINS &&
                hmje_worker_alloc_cache.count < HMJE_WORKER_CACHE_LIMIT &&
                capacity <= ((size_t)8 << 20) - hmje_worker_alloc_cache.bytes) {
                *(void **)pointer = hmje_worker_alloc_cache.heads[bin];
                hmje_worker_alloc_cache.heads[bin] = pointer;
                hmje_worker_alloc_cache.bytes += capacity;
                ++hmje_worker_alloc_cache.count;
                return;
            }
        }
        free(pointer);
        return;
    }
    if (pointer == NULL) {
        return;
    }
    hmje_tracked_allocation *allocation =
        ((hmje_tracked_allocation *)pointer) - 1;
    if (allocation->magic != HMJE_TRACKED_LIVE) {
        fprintf(
            stderr,
            "hmje-invalid-free=%p magic=0x%llx\n",
            pointer,
            (unsigned long long)allocation->magic
        );
#if defined(__APPLE__) || defined(__linux__)
        void *frames[64];
        int frame_count =
            backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
        backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
#endif
        abort();
    }
    pthread_mutex_lock(&hmje_track_lock);
    if (allocation->previous == NULL) {
        if (hmje_live_head != allocation) {
            fprintf(stderr, "hmje-invalid-live-list=%p\n", pointer);
            abort();
        }
        hmje_live_head = allocation->next;
    } else {
        allocation->previous->next = allocation->next;
    }
    if (allocation->next != NULL) {
        allocation->next->previous = allocation->previous;
    }
    allocation->magic = HMJE_TRACKED_FREED;
    hmje_live_allocations -= 1;
    allocation->next = NULL;
    allocation->previous = NULL;
#if defined(__APPLE__) || defined(__linux__)
    free(allocation->diagnostic_trace);
#endif
    allocation->diagnostic_trace = NULL;
    memset(pointer, 0xdd, (size_t)allocation->size);
    if (hmje_quarantine_limit == 0) {
        pthread_mutex_unlock(&hmje_track_lock);
        free(allocation);
        return;
    }
    if (hmje_quarantine_tail == NULL) {
        hmje_quarantine_head = allocation;
    } else {
        hmje_quarantine_tail->next = allocation;
    }
    hmje_quarantine_tail = allocation;
    if (allocation->size > UINT64_MAX - hmje_quarantine_bytes) {
        hmje_quarantine_bytes = UINT64_MAX;
    } else {
        hmje_quarantine_bytes += allocation->size;
    }
    hmje_quarantine_trim();
    pthread_mutex_unlock(&hmje_track_lock);
}

__attribute__((destructor))
static void hmje_report_allocation_balance(void) {
    const char *value = getenv("HMJE_REPORT_ALLOC_BALANCE");
    if (value != NULL && value[0] != '\0' && strcmp(value, "0") != 0) {
        fprintf(
            stderr,
            "hmje-live-allocations=%llu quarantine-bytes=%llu\n",
            (unsigned long long)hmje_live_allocations,
            (unsigned long long)hmje_quarantine_bytes
        );
    }
    const char *details = getenv("HMJE_REPORT_ALLOC_LIVE");
    if (details != NULL && details[0] != '\0' && strcmp(details, "0") != 0) {
        hmje_tracked_allocation *allocation = hmje_live_head;
        while (allocation != NULL) {
            fprintf(
                stderr,
                "hmje-live-allocation=%p size=%llu\n",
                (void *)(allocation + 1),
                (unsigned long long)allocation->size
            );
#if defined(__APPLE__) || defined(__linux__)
            hmje_allocation_trace *trace =
                (hmje_allocation_trace *)allocation->diagnostic_trace;
            if (trace != NULL && trace->frame_count > 0) {
                backtrace_symbols_fd(
                    trace->frames,
                    trace->frame_count,
                    STDERR_FILENO
                );
            }
#endif
            const char *words = getenv("HMJE_REPORT_ALLOC_WORDS");
            if (
                words != NULL && words[0] != '\0' && strcmp(words, "0") != 0
            ) {
                const uint64_t *payload = (const uint64_t *)(allocation + 1);
                size_t word_count = (size_t)allocation->size / sizeof(uint64_t);
                for (size_t index = 0; index < word_count; index += 1) {
                    uint64_t word = payload[index];
#if defined(__APPLE__)
                    Dl_info info;
                    memset(&info, 0, sizeof(info));
                    if (
                        word > 4096 &&
                        dladdr((const void *)(uintptr_t)word, &info) != 0 &&
                        info.dli_sname != NULL
                    ) {
                        fprintf(
                            stderr,
                            "hmje-live-word=%zu symbol=%s\n",
                            index,
                            info.dli_sname
                        );
                    } else
#endif
                    if (word <= 4096) {
                        fprintf(
                            stderr,
                            "hmje-live-word=%zu small=%llu\n",
                            index,
                            (unsigned long long)word
                        );
                    } else {
                        fprintf(
                            stderr,
                            "hmje-live-word=%zu opaque\n",
                            index
                        );
                    }
                }
            }
            allocation = allocation->next;
        }
    }
}

/* kernel/api/syscall.hmje 의 번호와 반드시 일치해야 한다. */
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
#define SYS_OBJECT_WRITE_AT 38
#define SYS_OBJECT_SYNC 42
#define SYS_FRAMEBUFFER_INFO 54
#define SYS_FRAMEBUFFER_BLIT 55
#define SYS_INPUT_POLL 56
#define SYS_TIME_US 60
#define SYS_SLEEP_US 61
#define SYS_FRAMEBUFFER_PRESENT 62
#define SYS_FILE_OPEN 75
#define SYS_FILE_LIST 76
#define SYS_SOCKET_CONNECT 85
#define SYS_SOCKET_SEND 86
#define SYS_SOCKET_RECV 87
#define SYS_GPU_COMMAND 90
#define SYS_FILE_LOCK 91
#define SYS_FILE_UNLINK 92
#define SYS_FILE_IDENTITY 94
#define SYS_WALL_TIME_S 95
#define SYS_INPUT_POLL2 98
#define SYS_UI_SYSTEM_THEME 110
#define SYS_TEXT_INPUT_CONTROL 104
#define SYS_TEXT_INPUT_CONTROL2 111
#define SYS_UI_WINDOW_CONFIG 112
#define SYS_UI_CLIPBOARD 113
#define SYS_UI_WINDOW 117
#define SYS_APP_OPEN_URL 120
#define SYS_UI_DRAG_DROP 140
#define SYS_WALL_TIME_MS 114
#define SYS_SECURE_RANDOM_FILL 115
#define SYS_LOCAL_DATETIME 116
#define SYS_UI_ACCESSIBILITY 109
#define SYS_AUDIO_CAPTURE_OPEN 105
#define SYS_AUDIO_CAPTURE_READ 106
#define SYS_AUDIO_CAPTURE_CONTROL 107
#define SYS_AUDIO_CAPTURE_PERMISSION 108
#define SYS_ARG_SIZES 99
#define SYS_ARG_READ 100
#define SYS_ENV_READ 101

/* kernel/api/syscall.hmje의 ERR_ENV_NOT_FOUND와 값이 반드시 일치해야 한다. */
#define ERR_ENV_NOT_FOUND 2

/* 범용 소켓 API 오류 코드(kernel/api/syscall.hmje의 ERR_SOCKET_*와 값이 반드시 일치해야
 * 한다). 다른 실패는 전부 기존 관례대로 HMJE_POSIX_ENOSYS로 뭉뚱그린다. */
#define ERR_SOCKET_TIMEOUT 110
#define ERR_SOCKET_CONN_REFUSED 111
#define ERR_SOCKET_NOT_CONNECTED 107
/* 비차단 읽기의 "아직 없음". 0(EOF)과 반드시 구분돼야 한다 —
 * library/kernel/api/syscall.hmje의 ERR_SOCKET_WOULD_BLOCK과 같은 값. */
#define ERR_SOCKET_WOULD_BLOCK 11
/* SYS_SOCKET_RECV의 네 번째 인자 플래그. */
#define SOCKET_RECV_NONBLOCK 1
#define ERR_SOCKET_HOST_NOT_FOUND 113

/* 타임아웃밀리초=0일 때 쓰는 기본값(runtime/user/socket.hmje의 소켓_기본타임아웃밀리초와 일치). */
#define SOCKET_DEFAULT_TIMEOUT_MS 10000

/* 범용 파일 API 모드 비트(kernel/api/syscall.hmje의 FILE_MODE_*와 일치). */
#define FILE_MODE_READ 1
#define FILE_MODE_WRITE 2
#define FILE_MODE_CREATE 4
#define FILE_MODE_EXCLUSIVE_CREATE 8

#define FILE_LOCK_SHARED 1
#define FILE_LOCK_EXCLUSIVE 2
#define FILE_LOCK_UNLOCK 3
#define FILE_LOCK_TRY 4
#define ERR_BUSY 16
#define ERR_ALREADY_EXISTS 17
#define ERR_INVALID_ARGUMENT 22

/* SYS_FILE_LIST 출력 레코드(64B, kernel/api/syscall.hmje의 FILE_LIST_*와 일치). */
#define FILE_LIST_ENTRY_SIZE 64
#define FILE_LIST_NAME_LEN_OFFSET 0
#define FILE_LIST_IS_DIR_OFFSET 8
#define FILE_LIST_SIZE_OFFSET 16
#define FILE_LIST_RESERVED_OFFSET 24
#define FILE_LIST_NAME_OFFSET 32
#define FILE_LIST_NAME_INLINE_MAX 32

/* SYS_OBJECT_STAT 레코드의 "features" 필드(오프셋 8) 비트 — OBJECT_FEATURE_HAS_LINKED_EDGES와
 * 값이 같다(그래프에서는 "자식 엣지가 있다" = 파일 API에서는 "디렉터리류"로 재사용). */
#define FILE_STAT_FEATURE_IS_DIR 2

/* 구현 안 된 syscall이 반환하는 명백히-실패 값(리눅스 ENOSYS와 같은 -38). */
#define HMJE_POSIX_ENOSYS (-38LL)

typedef long long i64;
typedef unsigned long long u64;

/*
 * 계산 작업자 풀 크기. Apple Silicon에서는 효율 코어를 제외한 성능 코어 수를 우선한다.
 * 대칭 SMT 호스트에서는 정수 양자 내적과 메모리 지연을 숨길 수 있도록 온라인 논리 CPU
 * 수를 돌려준다. 작업별로 물리 코어 수가 유리하면 호출자가 작업자 수를 명시할 수 있다.
 */
u64 hmje_platform_performance_cpu_count(void) {
#if defined(__APPLE__)
    uint32_t performance_count = 0;
    size_t size = sizeof(performance_count);
#if defined(__aarch64__) || defined(__arm64__)
    if (sysctlbyname("hw.perflevel0.logicalcpu", &performance_count, &size, NULL, 0) == 0 && performance_count > 0) {
        return (u64) performance_count;
    }
#endif
#endif
    long online_count = sysconf(_SC_NPROCESSORS_ONLN);
    return online_count > 0 ? (u64) online_count : 1;
}

/* 범용 파일 API(SYS_FILE_OPEN/LIST)의 기본 데이터 루트는 CWD/host_app_data다.
 * macOS .app도 번들 식별자 기반 Application Support를 자동 선택하지 않는다.
 * 호스트가 hmje_posix_set_file_data_dir로 절대 경로를 지정할 수 있다(iOS가 사용).
 * 설정·제약은 docs/posix-file-data-root.md에 기록한다. "/host/" 개발용 읽기전용
 * 다리(HMJE_POSIX_ENABLE_HOST_FILES)와는 완전히 별개다. */
#ifndef HMJE_POSIX_FILE_DATA_DIR
#define HMJE_POSIX_FILE_DATA_DIR "host_app_data"
#endif
/* 실행 시 바꿀 수 있는 데이터 루트. iOS 앱은 번들(읽기 전용) 대신 컨테이너의
 * Application Support/hmje-data를 지정한다(posix_ios_window.m). 루트로 시작하는
 * 절대 경로(예: 금고의 앱데이터경로 결과)는 그대로 통과시킨다. */
static char hmje_posix_file_data_root[4096] = HMJE_POSIX_FILE_DATA_DIR;
void hmje_posix_set_file_data_dir(const char *path) {
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(hmje_posix_file_data_root)) return;
    memcpy(hmje_posix_file_data_root, path, strlen(path) + 1);
}

/* posix_host_window.m (Cocoa) 구현. 인자/반환은 hmje_arch_syscall* 규약과 1:1. */
/* Optional host appearance. Headless/Linux hosts retain light unless explicitly set. */
__attribute__((weak)) i64 hmje_posix_system_theme(void) { return 0; }

extern i64 hmje_posix_framebuffer_info(i64 out_address, i64 out_length);
extern i64 hmje_posix_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y);
extern i64 hmje_posix_framebuffer_present(i64 rects_address, i64 count);
extern i64 hmje_posix_input_poll(void);
extern i64 hmje_posix_input_poll2(i64 out_address, i64 out_length);
__attribute__((weak)) i64 hmje_posix_ui_accessibility(i64 operation, i64 address, i64 length) {
    (void)operation; (void)address; (void)length;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_text_input_control2(i64 version, i64 client, i64 flags) {
    (void)version; (void)client; (void)flags;
    return HMJE_POSIX_ENOSYS; /* No insecure keyboard fallback. */
}
__attribute__((weak)) i64 hmje_posix_text_input_control(i64 active) {
    (void)active;
    return 0;
}
__attribute__((weak)) i64 hmje_posix_ui_window_config(i64 title_address, i64 title_length, i64 flags) {
    (void)title_address; (void)title_length; (void)flags;
    return 0;
}
__attribute__((weak)) i64 hmje_posix_ui_clipboard(i64 operation, i64 address, i64 length) {
    (void)operation; (void)address; (void)length;
    return HMJE_POSIX_ENOSYS;
}
/* 데스크톱 창 관리(SYS_UI_WINDOW). macOS는 posix_host_window.m이 구현하고, 창이
 * 없는 hosted POSIX(Linux headless, iOS)는 ENOSYS다. */
__attribute__((weak)) i64 hmje_posix_ui_window(i64 operation, i64 argument1, i64 argument2) {
    (void)operation; (void)argument1; (void)argument2;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_audio_capture_open(i64 config_address, i64 config_length) {
    (void)config_address;
    (void)config_length;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_audio_capture_read(i64 handle, i64 output_address, i64 output_capacity, i64 metadata_address, i64 metadata_length) {
    (void)handle; (void)output_address; (void)output_capacity;
    (void)metadata_address; (void)metadata_length;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_audio_capture_control(i64 handle, i64 command) {
    (void)handle; (void)command;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_audio_capture_permission(i64 action) {
    (void)action;
    return HMJE_POSIX_ENOSYS;
}
/* 앱을 연 주소의 큐(SYS_APP_OPEN_URL). macOS(posix_host_window.m)·iOS(posix_ios_window.m)가
 * 구현하고, 주소로 열리지 않는 hosted POSIX(Linux headless, 명령행)는 ENOSYS다. */
__attribute__((weak)) i64 hmje_posix_app_open_url(i64 operation, i64 address, i64 capacity) {
    (void)operation; (void)address; (void)capacity;
    return HMJE_POSIX_ENOSYS;
}
__attribute__((weak)) i64 hmje_posix_ui_drag_drop(i64 operation, i64 address, i64 capacity) {
    (void)operation; (void)address; (void)capacity;
    return HMJE_POSIX_ENOSYS;
}
extern i64 hmje_posix_gpu_command(i64 kind, i64 request_address, i64 request_length);

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
        return -(i64)errno;
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
#if defined(__APPLE__)
    arc4random_buf((void *)(uintptr_t)(u64)address, 8);
    return 8;
#elif defined(__linux__)
    unsigned char *out = (unsigned char *)(uintptr_t)(u64)address;
    size_t done = 0;
    while (done < 8) {
        ssize_t count = getrandom(out + done, 8 - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -(i64)errno : -5;
        done += (size_t)count;
    }
    return 8;
#else
    return -38;
#endif
}

int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms);

static int hmje_posix_plain_sleep_state = -1;
static int hmje_posix_plain_sleep_enabled(void) {
    if (hmje_posix_plain_sleep_state < 0) {
        hmje_posix_plain_sleep_state = getenv("HMJE_POSIX_DEBUG_PLAIN_SLEEP") != NULL;
    }
    return hmje_posix_plain_sleep_state;
}

static i64 sleep_ns(i64 ns) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000LL);
    req.tv_nsec = (long)(ns % 1000000000LL);
    while (nanosleep(&req, &req) != 0) {
        /* 시그널로 깨면 남은 시간만큼 다시 잔다. */
    }
    return 0;
}

/* ---- SYS_VM_RESERVE/RELEASE: munmap에 길이가 필요해서 주소→길이를 기록 ---- */

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
    void *addr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return 0;
    }
#if defined(__linux__) && defined(MADV_NOHUGEPAGE)
    /* Sparse 글꼴 atlas는 접근한 4 KiB 페이지만 필요하다. THP의 background
     * collapse가 빈 페이지까지 2 MiB로 채우면 유휴 중 RSS가 계속 늘어난다.
     * 일반 malloc/컴파일 작업에는 적용하지 않는다. */
    (void)madvise(addr, (size_t)size, MADV_NOHUGEPAGE);
#endif
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
    return HMJE_POSIX_ENOSYS;
}

/* ---- 제품 자원: 논리 이름 → 빌드가 놓은 자원 파일 ---------------------------------------
 * 표준의 자원 API(library/Sources/표준/resource.hmje)가 `hmje_resource_open`으로 여는
 * 논리 이름(`fonts/Pretendard-Regular.otf`, `unicode/…`)은 모듈 `자원 경로` 기준 상대
 * 경로이고, 빌드는 그 경로를 그대로 보존해 제품 자원 디렉터리에 복사한다
 * (compiler/docs/project.md 3.10·3.11). 이 파일은 그 디렉터리를 찾는다.
 *   - macOS/Linux 원시 실행 파일: `<실행 파일>.assets/<이름>`
 *   - macOS .app(HMJE_POSIX_MACOS_APP_BUNDLE): `Contents/Resources/assets/<이름>`
 *   - 프로젝트 그래프가 없는 단일 파일 개발 빌드(HMJE_POSIX_RESOURCES_DIR):
 *     제품 조정기가 박은 `HMJE_UI_ROOT/assets/<이름>`
 *   - iOS: posix_ios_window.m이 `hmje_posix_open_resource_file`을 앱 번들로 덮어쓴다.
 * 옛 `/system/fonts/{name}` graph walk는 `fonts/{name}` 자원의 별칭으로 남는다. */
static int hmje_resource_name_valid(const char *name, size_t name_len) {
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

__attribute__((weak)) i64 hmje_posix_open_resource_file(const char *name, size_t name_len) {
    (void)name_len;
    char full[4096];
#ifdef HMJE_POSIX_RESOURCES_DIR
    int written = snprintf(full, sizeof(full), "%s/%s", HMJE_POSIX_RESOURCES_DIR, name);
#else
    char executable[2048];
#if defined(__APPLE__)
    uint32_t executable_size = (uint32_t)sizeof(executable);
    if (_NSGetExecutablePath(executable, &executable_size) != 0) {
        return -ENOENT;
    }
#elif defined(__linux__)
    ssize_t executable_size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (executable_size <= 0 || (size_t)executable_size >= sizeof(executable)) {
        return -ENOENT;
    }
    executable[executable_size] = '\0';
#else
    return -ENOENT;
#endif
#if defined(HMJE_POSIX_MACOS_APP_BUNDLE)
#if defined(__APPLE__)
    char *last_separator = strrchr(executable, '/');
    if (last_separator == NULL) {
        return -ENOENT;
    }
    *last_separator = '\0';
    int written = snprintf(full, sizeof(full), "%s/../Resources/assets/%s", executable, name);
#else
    return -ENOENT;
#endif
#else
    int written = snprintf(full, sizeof(full), "%s.assets/%s", executable, name);
#endif
#endif
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

/* 표준 자원 API의 hosted 원천. 반환: fd 핸들(>0), -EINVAL(잘못된 이름), -ENOENT(없음). */
i64 hmje_resource_open(const char *name, u64 name_len) {
    if (!hmje_resource_name_valid(name, (size_t)name_len)) {
        return -EINVAL;
    }
    char copy[1025];
    memcpy(copy, name, (size_t)name_len);
    copy[name_len] = '\0';
    return hmje_posix_open_resource_file(copy, (size_t)name_len);
}

/* 호환 폰트 훅. `/system/fonts/{rel}`은 `fonts/{rel}` 자원이다. */
__attribute__((weak)) i64 hmje_posix_open_font_file(const char *rel, size_t rel_len) {
    char name[1025];
    static const char fonts_prefix[] = "fonts/";
    const size_t fonts_prefix_len = sizeof(fonts_prefix) - 1;
    if (rel_len == 0 || rel_len + fonts_prefix_len > 1024) {
        return HMJE_POSIX_ENOSYS;
    }
    memcpy(name, fonts_prefix, fonts_prefix_len);
    memcpy(name + fonts_prefix_len, rel, rel_len);
    name[fonts_prefix_len + rel_len] = '\0';
    if (!hmje_resource_name_valid(name, fonts_prefix_len + rel_len)) {
        return HMJE_POSIX_ENOSYS;
    }
    i64 handle = hmje_posix_open_resource_file(name, fonts_prefix_len + rel_len);
    return handle > 0 ? handle : HMJE_POSIX_ENOSYS;
}

static i64 object_open_graph_walk(i64 path_addr, i64 path_len) {
    static const char font_prefix[] = "/system/fonts/";
    const size_t font_prefix_len = sizeof(font_prefix) - 1;
    if (path_len <= 0) {
        return HMJE_POSIX_ENOSYS;
    }
    const char *path_bytes = (const char *)(uintptr_t)(u64)path_addr;
    if ((u64)path_len > font_prefix_len && memcmp(path_bytes, font_prefix, font_prefix_len) == 0) {
        size_t rel_len = (size_t)path_len - font_prefix_len;
        char rel[256];
        if (rel_len == 0 || rel_len >= sizeof(rel)) {
            return HMJE_POSIX_ENOSYS;
        }
        memcpy(rel, path_bytes + font_prefix_len, rel_len);
        rel[rel_len] = '\0';
        if (strstr(rel, "..") != NULL) {
            return HMJE_POSIX_ENOSYS;
        }
        return hmje_posix_open_font_file(rel, rel_len);
    }

#ifdef HMJE_POSIX_ENABLE_HOST_FILES
    static const char host_prefix[] = "/host/";
    const size_t host_prefix_len = sizeof(host_prefix) - 1;
    if ((u64)path_len > host_prefix_len && memcmp(path_bytes, host_prefix, host_prefix_len) == 0) {
        size_t rel_len = (size_t)path_len - host_prefix_len;
        if (rel_len == 0 || rel_len >= 4096) {
            return HMJE_POSIX_ENOSYS;
        }
        const char *rel_bytes = path_bytes + host_prefix_len;
        if (memchr(rel_bytes, '\0', rel_len) != NULL) {
            return HMJE_POSIX_ENOSYS;
        }

        /* CWD 탈출만 막는다. 이름 안의 연속 점은 허용하고 성분이 정확히 '..'일 때만 거절. */
        size_t component_start = 0;
        for (size_t i = 0; i <= rel_len; ++i) {
            if (i != rel_len && rel_bytes[i] != '/') {
                continue;
            }
            size_t component_len = i - component_start;
            if (component_len == 0 ||
                (component_len == 2 && rel_bytes[component_start] == '.' && rel_bytes[component_start + 1] == '.')) {
                return HMJE_POSIX_ENOSYS;
            }
            component_start = i + 1;
        }

        char rel[4096];
        memcpy(rel, rel_bytes, rel_len);
        rel[rel_len] = '\0';
        int fd = open(rel, O_RDONLY);
        if (fd < 0) {
            return HMJE_POSIX_ENOSYS;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            return HMJE_POSIX_ENOSYS;
        }
        return (i64)fd;
    }
#endif

    return HMJE_POSIX_ENOSYS;
}

static i64 object_stat(i64 fd_handle, i64 out_addr, i64 out_len) {
    if (fd_handle <= 0 || out_len < 64) {
        return HMJE_POSIX_ENOSYS;
    }
    struct stat st;
    if (fstat((int)fd_handle, &st) != 0) {
        return HMJE_POSIX_ENOSYS;
    }
    /* runtime/user/object.hmje의 객체메타데이터가 읽는 필드만 채운다: idx0=식별자,
     * idx3(오프셋24)=크기, idx5(오프셋40)=형식(미사용, 0). 나머지는 0.
     * idx1(오프셋8, "features")은 범용 파일 API(runtime/user/file.hmje)의 파일정보가
     * 디렉터리 여부 판단에 쓴다 — semtleOS 쪽 hmje_object_node_features와 값을 맞춘다. */
    u64 record[8];
    memset(record, 0, sizeof(record));
    record[0] = (u64)fd_handle;
    if (S_ISDIR(st.st_mode)) {
        record[1] = FILE_STAT_FEATURE_IS_DIR;
    }
    record[3] = (u64)st.st_size;
    memcpy((void *)(uintptr_t)(u64)out_addr, record, sizeof(record));
    return 0;
}

/* SYS_FILE_IDENTITY(94): object-stat의 SemtleOS metadata 레코드를 재해석하지 않고,
 * hosted filesystem에서만 열린 개체의 stable device/inode 쌍을 별도 ABI로 낸다. */
static i64 file_identity(i64 handle, i64 out_addr, i64 out_length) {
    struct stat st;
    u64 identity[2];
    if (handle <= 0) return -(i64)EBADF;
    if (out_addr == 0 || out_length < (i64)sizeof(identity)) {
        return -(i64)EINVAL;
    }
    if (fstat((int)handle, &st) != 0) {
        return -(i64)(errno ? errno : EIO);
    }
    identity[0] = (u64)st.st_dev;
    identity[1] = (u64)st.st_ino;
    memcpy((void *)(uintptr_t)(u64)out_addr, identity, sizeof(identity));
    return 0;
}

/* SYS_OBJECT_READ_AT(37): 지정 오프셋에서 읽되 handle의 streaming offset(파일 포인터)은
 * 바꾸지 않는다(docs/syscall.md READ_AT 계약). POSIX의 pread가 정확히 이 의미라 그대로 쓴다
 * — Windows shim이 ReadFile+파일 포인터 저장/복원으로 흉내낸 것과 달리 커널이 원자적으로
 * 보장해준다. flags/reserved는 0만 허용. */
static i64 object_read_at(i64 handle, i64 offset, i64 out_addr, i64 length, i64 flags, i64 reserved) {
    if (handle <= 0 || offset < 0 || length < 0 || flags != 0 || reserved != 0) {
        return -(i64)EINVAL;
    }
    ssize_t n = pread((int)handle, (void *)(uintptr_t)(u64)out_addr, (size_t)length, (off_t)offset);
    return n < 0 ? -(i64)errno : (i64)n;
}

/* SYS_OBJECT_WRITE_AT(38): object_read_at과 대칭인 pwrite. 범용 파일 API의 파일쓰기가
 * 이 번호를 그대로 쓴다(runtime/user/file.hmje). handle의 streaming offset은 안 바꾼다. */
static i64 object_write_at(i64 handle, i64 offset, i64 in_addr, i64 length, i64 flags, i64 reserved) {
    if (handle <= 0 || offset < 0 || length < 0 || flags != 0 || reserved != 0) {
        return -(i64)EINVAL;
    }
    ssize_t n = pwrite((int)handle, (const void *)(uintptr_t)(u64)in_addr, (size_t)length, (off_t)offset);
    return n < 0 ? -(i64)errno : (i64)n;
}

/* regular file과 parent directory가 같은 물리 저장 경계를 쓰도록 공통화한다.
 * EINTR만 다시 시도하고 EIO/ENOSPC 같은 원인은 보존한다. */
static i64 durable_sync_fd(int fd) {
#if defined(__APPLE__)
    int full_result;
    do {
        full_result = fcntl(fd, F_FULLFSYNC);
    } while (full_result != 0 && errno == EINTR);
    if (full_result == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOTSUP) {
        return -(i64)errno;
    }
#endif
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? 0 : -(i64)errno;
}

/* SYS_OBJECT_SYNC(42): semtleOS에서는 object/STFS commit, hosted에서는 같은 핸들의
 * POSIX durability barrier다. */
static i64 object_sync(i64 handle) {
    if (handle <= 0) {
        return -(i64)EBADF;
    }
    return durable_sync_fd((int)handle);
}

/* ---- SYS_FILE_OPEN/SYS_FILE_LIST/SYS_FILE_UNLINK: 범용 파일 API 뒷단 ------------------- */
/* object_open_graph_walk의 "/host/" 다리(HMJE_POSIX_ENABLE_HOST_FILES, 읽기전용, 개발용
 * fixture)와는 완전히 별개다 — 이쪽은 항상 켜져 있고 읽기+쓰기+생성+목록을 전부 지원하며,
 * 경로는 앱 전용 데이터 디렉터리(HMJE_POSIX_FILE_DATA_DIR) 기준이다. */

/* full[out_cap] 에 "HMJE_POSIX_FILE_DATA_DIR/<경로에서 맨 앞 '/'을 뺀 나머지>"를 채운다.
 * ".." 성분이나 빈 성분(연속 슬래시)이 있으면 거부한다. */
static i64 file_resolve_path(i64 path_addr, i64 path_len, char *full, size_t full_cap) {
    if (path_len <= 0 || (size_t)path_len >= 4096) {
        return HMJE_POSIX_ENOSYS;
    }
    const char *bytes = (const char *)(uintptr_t)(u64)path_addr;
    if (bytes[0] != '/') {
        return HMJE_POSIX_ENOSYS;
    }
    if (memchr(bytes, '\0', (size_t)path_len) != NULL) {
        return HMJE_POSIX_ENOSYS;
    }

    /* 데이터 루트 자체 또는 그 아래의 절대 경로는 그대로 쓴다. */
    {
        size_t root_len = strlen(hmje_posix_file_data_root);
        if (hmje_posix_file_data_root[0] == '/' && (size_t)path_len >= root_len &&
            memcmp(bytes, hmje_posix_file_data_root, root_len) == 0 &&
            ((size_t)path_len == root_len || bytes[root_len] == '/')) {
            if ((size_t)path_len >= full_cap) return HMJE_POSIX_ENOSYS;
            for (i64 i = 0; i < path_len; ++i) {
                if (bytes[i] == '/' && i + 2 < path_len && bytes[i + 1] == '.' && bytes[i + 2] == '.' &&
                    (i + 3 == path_len || bytes[i + 3] == '/')) return HMJE_POSIX_ENOSYS;
            }
            memcpy(full, bytes, (size_t)path_len);
            full[path_len] = '\0';
            return 0;
        }
    }
    const char *rel_bytes = bytes + 1;
    size_t rel_len = (size_t)path_len - 1;

    /* CWD/데이터루트 탈출만 막는다(기존 /host/ 다리와 같은 규칙): 성분이 정확히 '..'이거나
     * 비어있으면(연속 슬래시) 거절한다. */
    size_t component_start = 0;
    for (size_t i = 0; i <= rel_len; ++i) {
        if (i != rel_len && rel_bytes[i] != '/') {
            continue;
        }
        size_t component_len = i - component_start;
        if (component_len == 0 ||
            (component_len == 2 && rel_bytes[component_start] == '.' && rel_bytes[component_start + 1] == '.')) {
            return HMJE_POSIX_ENOSYS;
        }
        component_start = i + 1;
    }

    int written = snprintf(full, full_cap, "%s/%.*s", hmje_posix_file_data_root, (int)rel_len, rel_bytes);
    if (written <= 0 || (size_t)written >= full_cap) {
        return HMJE_POSIX_ENOSYS;
    }
    return 0;
}

static i64 file_sync_directory_path(const char *path) {
    int directory_fd = open(path, O_RDONLY);
    if (directory_fd < 0) {
        return -(i64)errno;
    }
    i64 sync_result = durable_sync_fd(directory_fd);
    int close_result = close(directory_fd);
    int close_errno = errno;
    if (sync_result != 0) {
        return sync_result;
    }
    return close_result == 0 ? 0 : -(i64)close_errno;
}

/* path 자체가 새 directory일 때 그 이름을 담는 parent를 동기화한다. */
static i64 file_sync_parent_of_path(char *path) {
    char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        return file_sync_directory_path(".");
    }
    if (last_slash == path) {
        return file_sync_directory_path("/");
    }

    *last_slash = '\0';
    i64 result = file_sync_directory_path(path);
    *last_slash = '/';
    return result;
}

/* full의 마지막 '/' 앞까지 mkdir -p 한다. 새 data-root/중간 directory의 이름도
 * parent에 순서대로 영속화해 마지막 파일 entry만 살아남고 조상이 빠지는 틈을 막는다. */
static i64 file_ensure_parent_dirs(char *full) {
    char *last_slash = strrchr(full, '/');
    if (last_slash == NULL) {
        return -(i64)EINVAL;
    }
    size_t data_root_len = strlen(hmje_posix_file_data_root);
    /* 절대 데이터 루트는 호스트가 생성한다.
     * iOS sandbox 밖의 상위 성분(/private/var/...)에는 mkdir을 시도하지 않는다. */

    char *scan_start = full + 1;
    if (hmje_posix_file_data_root[0] == '/' && strncmp(full, hmje_posix_file_data_root, data_root_len) == 0 &&
        full[data_root_len] == '/') {
        scan_start = full + data_root_len + 1;
    }
    for (char *p = scan_start; p < last_slash; ++p) {
        if (*p == '/') {
            *p = '\0';
            int mkdir_result = mkdir(full, 0700);
            int mkdir_errno = errno;
            size_t prefix_len = (size_t)(p - full);
            if (mkdir_result != 0 && mkdir_errno != EEXIST) {
                *p = '/';
                return -(i64)mkdir_errno;
            }
            if (mkdir_result == 0 || prefix_len >= data_root_len) {
                i64 parent_sync_result = file_sync_parent_of_path(full);
                if (parent_sync_result != 0) {
                    *p = '/';
                    return parent_sync_result;
                }
            }
            *p = '/';
        }
    }
    *last_slash = '\0';
    int mkdir_result = mkdir(full, 0700);
    int mkdir_errno = errno;
    size_t prefix_len = (size_t)(last_slash - full);
    if (mkdir_result != 0 && mkdir_errno != EEXIST) {
        *last_slash = '/';
        return -(i64)mkdir_errno;
    }
    if (mkdir_result == 0 || prefix_len >= data_root_len) {
        i64 parent_sync_result = file_sync_parent_of_path(full);
        if (parent_sync_result != 0) {
            *last_slash = '/';
            return parent_sync_result;
        }
    }
    *last_slash = '/';
    return 0;
}

/* O_EXCL로 만든 새 DB 이름도 파일 내용과 별도로 directory entry를 영속화해야 한다.
 * 호출 중에만 parent dirfd를 열어 barrier를 수행하므로 fd/path 추적 테이블은 필요 없다. */
static i64 file_sync_parent_directory(char *full) {
    char *last_slash = strrchr(full, '/');
    if (last_slash == NULL) {
        return -(i64)EINVAL;
    }

    *last_slash = '\0';
    i64 result = file_sync_directory_path(full);
    *last_slash = '/';
    return result;
}

/* 이미 resolve된 일반 파일 pathname을 제거하고 namespace 변경을 동기화한다. open의
 * 실패 정리에서는 반환값을 버리고 최초 durability 오류를 보존하며, 공개 unlink
 * syscall에서는 이 결과 자체가 호출 결과다. */
static i64 file_unlink_resolved(char *full) {
    int result;
    do {
        result = unlink(full);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return -(i64)errno;
    }
    return file_sync_parent_directory(full);
}

static i64 file_open(i64 path_addr, i64 path_len, i64 mode) {
    if ((mode & ~(FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE_CREATE)) != 0 ||
        (mode & (FILE_MODE_READ | FILE_MODE_WRITE)) == 0 ||
        ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0 && (mode & FILE_MODE_CREATE) == 0)) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    char full[4160];
    i64 resolve_result = file_resolve_path(path_addr, path_len, full, sizeof(full));
    if (resolve_result != 0) {
        return resolve_result;
    }

    int flags = ((mode & FILE_MODE_WRITE) != 0) ? O_RDWR : O_RDONLY;

    if ((mode & FILE_MODE_CREATE) != 0) {
        i64 parent_dirs_result = file_ensure_parent_dirs(full);
        if (parent_dirs_result != 0) {
            return parent_dirs_result;
        }
        flags |= O_CREAT;
        if ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0) {
            flags |= O_EXCL;
        }
    }

    int fd = open(full, flags, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return -(i64)ERR_ALREADY_EXISTS;
        }
        return -(i64)errno;
    }
    /* HMJE의 기존 파일 핸들 계약은 0을 실패로 예약한다. 부모가 표준 fd를 닫고 실행한
     * 특수 환경에서도 open() 결과가 0이 되지 않도록 실제 fd를 3 이상으로 승격한다. */
    if (fd < 3) {
        int promoted = fcntl(fd, F_DUPFD, 3);
        int promote_errno = errno;
        close(fd);
        if (promoted < 0) {
            return -(i64)promote_errno;
        }
        fd = promoted;
    }

    /* DB/compact가 쓰는 배타 생성은 성공 반환 전에 새 pathname까지 crash-durable하게
     * 만든다. 이후 SYS_OBJECT_SYNC가 파일 내용/metadata 자체의 barrier를 담당한다. */
    if ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0) {
        i64 parent_sync_result = file_sync_parent_directory(full);
        if (parent_sync_result != 0) {
            /* O_EXCL 성공으로 이 호출이 만든 이름만 정리한다. 기존 경로를 연 경우에는
             * 이 분기에 올 수 없다. close/unlink 실패보다 최초 durability 오류가 우선이다. */
            (void)close(fd);
            (void)file_unlink_resolved(full);
            return parent_sync_result;
        }
    }
    return (i64)fd;
}

/* SYS_FILE_UNLINK(92): SYS_FILE_OPEN과 동일한 root/path validator를 거쳐 pathname만
 * 제거한다. unlink(2)는 디렉터리를 지우지 않으므로 directory lifecycle API와 섞이지
 * 않는다. 성공한 namespace 변경은 반환 전에 parent directory에도 durability barrier를
 * 건다. barrier 실패 시 이름은 이미 제거됐을 수 있으므로 호출자는 오류 뒤 재조회해야 한다. */
static i64 file_unlink(i64 path_addr, i64 path_len, i64 reserved) {
    if (reserved != 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }

    char full[4160];
    i64 resolve_result = file_resolve_path(path_addr, path_len, full, sizeof(full));
    if (resolve_result != 0) {
        return resolve_result;
    }

    return file_unlink_resolved(full);
}

/* SYS_FILE_LOCK(91): open file description 수명에 결합되는 BSD flock을 쓴다. 프로세스
 * 단위인 fcntl record lock은 같은 프로세스의 독립 open끼리 충돌하지 않고, 같은 inode의
 * 다른 fd 하나만 닫아도 잠금이 풀릴 수 있어 핸들 수명 잠금 계약에 맞지 않는다. */
static i64 file_lock(i64 handle, i64 mode, i64 reserved) {
    if (handle <= 0) {
        return -(i64)EBADF;
    }
    if (reserved != 0 || (mode & ~(i64)7) != 0) {
        return -(i64)EINVAL;
    }

    i64 operation = mode & 3;
    int lock_operation;
    if (operation == FILE_LOCK_SHARED) {
        lock_operation = LOCK_SH;
    } else if (operation == FILE_LOCK_EXCLUSIVE) {
        lock_operation = LOCK_EX;
    } else if (operation == FILE_LOCK_UNLOCK) {
        lock_operation = LOCK_UN;
    } else {
        return -(i64)EINVAL;
    }

    if ((mode & FILE_LOCK_TRY) != 0) {
        lock_operation |= LOCK_NB;
    }
    int result;
    do {
        result = flock((int)handle, lock_operation);
    } while (result != 0 && errno == EINTR && (mode & FILE_LOCK_TRY) == 0);
    if (result == 0) {
        return 0;
    }
    if (errno == EACCES || errno == EAGAIN || errno == EWOULDBLOCK) {
        return -(i64)ERR_BUSY; /* 플랫폼별 EAGAIN 번호를 ABI에서 정규화한다. */
    }
    return -(i64)errno;
}

/* dirfd를 dup해 fdopendir에 넘긴다 — closedir이 넘겨받은 fd를 닫아버리므로, 호출자가
 * 자기 핸들로 나중에 파일닫기(SYS_CLOSE)를 또 부를 수 있도록 원본 fd는 보존한다. */
static i64 file_list(i64 handle, i64 out_addr, i64 max_count) {
    if (handle <= 0) {
        return -(i64)EBADF;
    }
    if (max_count < 0 || (u64)max_count > (u64)(SIZE_MAX / FILE_LIST_ENTRY_SIZE)) {
        return -(i64)EINVAL;
    }
    if (max_count == 0) {
        return 0;
    }
    if (out_addr == 0) {
        return -(i64)EFAULT;
    }

    int dup_fd;
    do {
        dup_fd = dup((int)handle);
    } while (dup_fd < 0 && errno == EINTR);
    if (dup_fd < 0) {
        return -(i64)errno;
    }
    DIR *dir = fdopendir(dup_fd);
    if (dir == NULL) {
        int open_errno = errno;
        close(dup_fd);
        return -(i64)open_errno;
    }

    unsigned char *out = (unsigned char *)(uintptr_t)(u64)out_addr;
    i64 count = 0;
    i64 list_error = 0;

    /* API에 cursor가 없으므로 SemtleOS와 같이 매 호출 snapshot의 첫 항목부터 읽는다. */
    rewinddir(dir);
    while (count < max_count) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                list_error = -(i64)errno;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t name_len = strlen(entry->d_name);
        size_t copy_len = name_len > FILE_LIST_NAME_INLINE_MAX ? (size_t)FILE_LIST_NAME_INLINE_MAX : name_len;

        int is_dir = 0;
        struct stat st;
        if (fstatat(dirfd(dir), entry->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode)) {
            is_dir = 1;
        }

        unsigned char *record = out + (size_t)count * FILE_LIST_ENTRY_SIZE;
        u64 name_len_value = (u64)name_len;
        u64 is_dir_value = (u64)is_dir;
        u64 zero = 0;
        memcpy(record + FILE_LIST_NAME_LEN_OFFSET, &name_len_value, sizeof(name_len_value));
        memcpy(record + FILE_LIST_IS_DIR_OFFSET, &is_dir_value, sizeof(is_dir_value));
        /* +16은 모든 port에서 예약 필드다. 실제 크기는 SYS_OBJECT_STAT으로 읽는다. */
        memcpy(record + FILE_LIST_SIZE_OFFSET, &zero, sizeof(zero));
        memcpy(record + FILE_LIST_RESERVED_OFFSET, &zero, sizeof(zero));
        memset(record + FILE_LIST_NAME_OFFSET, 0, FILE_LIST_NAME_INLINE_MAX);
        memcpy(record + FILE_LIST_NAME_OFFSET, entry->d_name, copy_len);

        count++;
    }

    int close_result = closedir(dir); /* dup_fd도 여기서 같이 닫힌다 */
    int close_errno = errno;
    if (list_error != 0) {
        return list_error;
    }
    if (close_result != 0) {
        return -(i64)close_errno;
    }
    return count;
}

/* SYS_SOCKET_CONNECT/SEND/RECV: 범용 소켓 API(runtime/user/socket.hmje)의 POSIX 백엔드.
 * hosted(macOS/iOS/Android) 프로세스에서 TCP 클라이언트 소켓으로 동작한다.
 * 리스닝 소켓과 UDP는 지원하지 않는다. */


/* 실패 사유를 성공 경로와 분리해 추적한다 — 여러 addrinfo 후보를 순회하다 보면 마지막
 * 실패 사유가 가장 유의미하므로(예: IPv6는 네트워크 자체가 없어 즉시 실패, IPv4가 진짜
 * 원인) 매 시도마다 갱신한다. */
enum socket_connect_failure {
    SOCKET_CONNECT_FAIL_OTHER = 0,
    SOCKET_CONNECT_FAIL_REFUSED,
    SOCKET_CONNECT_FAIL_TIMEOUT,
};

static i64 socket_connect(i64 host_addr, i64 host_len, i64 port, i64 timeout_ms) {
    if (host_len <= 0 || host_len >= 256 || port <= 0 || port > 65535) {
        return HMJE_POSIX_ENOSYS;
    }
    const char *host_bytes = (const char *)(uintptr_t)(u64)host_addr;
    if (memchr(host_bytes, '\0', (size_t)host_len) != NULL) {
        return HMJE_POSIX_ENOSYS;
    }
    char host[256];
    memcpy(host, host_bytes, (size_t)host_len);
    host[host_len] = '\0';

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%lld", (long long)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -(i64)ERR_SOCKET_HOST_NOT_FOUND;
    }

    int effective_timeout_ms = (int)(timeout_ms > 0 ? timeout_ms : SOCKET_DEFAULT_TIMEOUT_MS);
    enum socket_connect_failure last_failure = SOCKET_CONNECT_FAIL_OTHER;

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
#ifdef SO_NOSIGPIPE
        /* macOS: send()에 MSG_NOSIGNAL이 없어서 대신 소켓 옵션으로 SIGPIPE를 막는다. */
        int nosigpipe_on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe_on, sizeof(nosigpipe_on));
#endif

        int original_flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, original_flags | O_NONBLOCK);

        int connect_result = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (connect_result == 0) {
            fcntl(fd, F_SETFL, original_flags);
            freeaddrinfo(res);
            return (i64)fd;
        }
        if (errno != EINPROGRESS) {
            last_failure = (errno == ECONNREFUSED) ? SOCKET_CONNECT_FAIL_REFUSED : SOCKET_CONNECT_FAIL_OTHER;
            close(fd);
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int poll_result = poll(&pfd, 1, effective_timeout_ms);
        if (poll_result == 0) {
            last_failure = SOCKET_CONNECT_FAIL_TIMEOUT;
            close(fd);
            continue;
        }
        if (poll_result < 0) {
            last_failure = SOCKET_CONNECT_FAIL_OTHER;
            close(fd);
            continue;
        }

        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 || so_error != 0) {
            last_failure = (so_error == ECONNREFUSED) ? SOCKET_CONNECT_FAIL_REFUSED : SOCKET_CONNECT_FAIL_OTHER;
            close(fd);
            continue;
        }

        fcntl(fd, F_SETFL, original_flags);
        freeaddrinfo(res);
        return (i64)fd;
    }

    freeaddrinfo(res);
    switch (last_failure) {
    case SOCKET_CONNECT_FAIL_REFUSED:
        return -(i64)ERR_SOCKET_CONN_REFUSED;
    case SOCKET_CONNECT_FAIL_TIMEOUT:
        return -(i64)ERR_SOCKET_TIMEOUT;
    default:
        return HMJE_POSIX_ENOSYS;
    }
}

/* SYS_SOCKET_SEND(86): send()의 얇은 래퍼. MSG_NOSIGNAL(있는 플랫폼)로 피어가 이미 닫은
 * 연결에 쓸 때 프로세스가 SIGPIPE로 죽는 것을 막는다(macOS는 connect 시점의 SO_NOSIGPIPE가
 * 이미 같은 역할을 한다). */
static i64 socket_send(i64 handle, i64 in_addr, i64 length) {
    if (handle <= 0 || length < 0) {
        return HMJE_POSIX_ENOSYS;
    }
    if (length == 0) {
        return 0;
    }
#ifdef MSG_NOSIGNAL
    ssize_t n = send((int)handle, (const void *)(uintptr_t)(u64)in_addr, (size_t)length, MSG_NOSIGNAL);
#else
    ssize_t n = send((int)handle, (const void *)(uintptr_t)(u64)in_addr, (size_t)length, 0);
#endif
    if (n < 0) {
        if (errno == ENOTCONN || errno == EPIPE) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_POSIX_ENOSYS;
    }
    return (i64)n;
}

/* SYS_SOCKET_RECV(87): recv()의 얇은 래퍼. 반환 0은 피어가 정상 종료(EOF)한 것으로, 오류가
 * 아니라 그대로 통과시킨다(소켓읽기의 문서화된 계약).
 *
 * flags에 SOCKET_RECV_NONBLOCK이 있으면 MSG_DONTWAIT으로 즉시 돌아온다. 자료가
 * 아직 없으면 EAGAIN/EWOULDBLOCK이 오는데, 이걸 **0으로 접으면 안 된다** — 0은
 * EOF고 이건 "조금 있다 다시"라서, 섞이면 스트림이 조기 종료되거나 영원히
 * 대기한다. 그래서 전용 코드로 돌려준다(커널 백엔드와 같은 규약). */
static i64 socket_recv(i64 handle, i64 out_addr, i64 length, i64 flags) {
    if (handle <= 0 || length < 0) {
        return HMJE_POSIX_ENOSYS;
    }
    if (length == 0) {
        return 0;
    }
    int recv_flags = 0;
    if ((flags & SOCKET_RECV_NONBLOCK) != 0) {
        recv_flags |= MSG_DONTWAIT;
    }
    ssize_t n = recv((int)handle, (void *)(uintptr_t)(u64)out_addr, (size_t)length, recv_flags);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -(i64)ERR_SOCKET_WOULD_BLOCK;
        }
        if (errno == ENOTCONN) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_POSIX_ENOSYS;
    }
    return (i64)n;
}

/* ---- SYS_SOCKET_LISTEN/ACCEPT, 비차단 송신, 비동기 준비 대기: 듣기(서버) 소켓 ---------- */
#define HMJE_SOCKET_SERVER_ENOSYS HMJE_POSIX_ENOSYS
#include "posix_socket_server.inc"

/* ---- 명령줄 인자 / 환경변수 (SYS_ARG_*, SYS_ENV_READ) --------------------- */

/* 생성된 IR의 main()은 인자를 받지 않으므로 real argv는 여기서 잡는다.
 * macOS는 crt_externs, Linux(glibc/musl)는 constructor가 argc/argv를 받는
 * 로더 계약을 쓴다. */
#if defined(__APPLE__)
static int hmje_host_argc(void) { return *_NSGetArgc(); }
static char **hmje_host_argv(void) { return *_NSGetArgv(); }
#else
static int hmje_saved_argc = 0;
static char **hmje_saved_argv = NULL;
__attribute__((constructor)) static void hmje_capture_args(int argc, char **argv, char **envp) {
    (void)envp;
    hmje_saved_argc = argc;
    hmje_saved_argv = argv;
}
static int hmje_host_argc(void) { return hmje_saved_argc; }
static char **hmje_host_argv(void) { return hmje_saved_argv; }
#endif

static u64 hmje_arg_total_bytes(void) {
    int argc = hmje_host_argc();
    char **argv = hmje_host_argv();
    u64 total = 0;
    for (int i = 0; i < argc && argv != NULL; i++) {
        total += (u64)strlen(argv[i]) + 1;
    }
    return total;
}

static i64 arg_sizes(i64 out_addr) {
    if (out_addr == 0) {
        return HMJE_POSIX_ENOSYS;
    }
    u64 *out = (u64 *)(uintptr_t)(u64)out_addr;
    out[0] = (u64)hmje_host_argc();
    out[1] = hmje_arg_total_bytes();
    return 0;
}

static i64 arg_read(i64 out_addr, i64 capacity) {
    u64 total = hmje_arg_total_bytes();
    if (out_addr == 0 || capacity < 0 || (u64)capacity < total) {
        return HMJE_POSIX_ENOSYS;
    }
    char *dst = (char *)(uintptr_t)(u64)out_addr;
    int argc = hmje_host_argc();
    char **argv = hmje_host_argv();
    for (int i = 0; i < argc && argv != NULL; i++) {
        size_t len = strlen(argv[i]) + 1;
        memcpy(dst, argv[i], len);
        dst += len;
    }
    return (i64)total;
}

static i64 env_read(i64 name_addr, i64 name_len, i64 out_addr, i64 capacity) {
    if (name_addr == 0 || name_len <= 0 || name_len > 4096) {
        return HMJE_POSIX_ENOSYS;
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
    case SYS_WRITE:
        return (i64)write((int)a1, (const void *)(uintptr_t)(u64)a2, (size_t)a3);
    case SYS_READ:
        return (i64)read((int)a1, (void *)(uintptr_t)(u64)a2, (size_t)a3);
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
         * 완료가 없으면 요청한 시간을 그대로 잔다(비동기 작업이 없는 프로그램은 변화 없음).
         * HMJE_POSIX_DEBUG_PLAIN_SLEEP=1은 진단용으로 고정 nanosleep을 사용한다. */
        if (a1 <= 0) return 0;
        if (hmje_posix_plain_sleep_enabled()) return sleep_ns(a1 * 1000000LL);
        /* 반환값: 0 시간 만료, bit0 native 완료로 조기 깨움, bit1 화면 주사(vsync) 도착. */
        return (i64)hmje_async_sleep_until_wake_ms((uint64_t)a1);
    case SYS_SLEEP_US:
        return sleep_ns(a1 * 1000LL);
    case SYS_ALLOC: {
        void *block = hmje_host_allocate((size_t)(u64)a1);
        return block == NULL ? 0 : (i64)(u64)(uintptr_t)block;
    }
    case SYS_FREE:
        hmje_host_release((void *)(uintptr_t)(u64)a1);
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
        return close((int)a1) == 0 ? 0 : -(i64)errno;
    case SYS_OBJECT_STAT:
        return object_stat(a1, a2, a3);
    case SYS_OBJECT_READ_AT:
        return object_read_at(a1, a2, a3, a4, a5, a6);
    case SYS_OBJECT_WRITE_AT:
        return object_write_at(a1, a2, a3, a4, a5, a6);
    case SYS_OBJECT_SYNC:
        return object_sync(a1);
    case SYS_FILE_OPEN:
        return file_open(a1, a2, a3);
    case SYS_FILE_LIST:
        return file_list(a1, a2, a3);
    case SYS_FILE_LOCK:
        return file_lock(a1, a2, a3);
    case SYS_FILE_UNLINK:
        return file_unlink(a1, a2, a3);
    case SYS_FILE_IDENTITY:
        return file_identity(a1, a2, a3);
    case SYS_SOCKET_CONNECT:
        return socket_connect(a1, a2, a3, a4);
    case SYS_SOCKET_SEND:
        return a4 == 0 ? socket_send(a1, a2, a3) : hmje_socket_send_flags(a1, a2, a3, a4);
    case SYS_SOCKET_LISTEN:
        return hmje_socket_listen(a1, a2, a3, a4, a5, a6);
    case SYS_SOCKET_ACCEPT:
        return hmje_socket_accept(a1, a2);
    case SYS_SOCKET_RECV:
        return socket_recv(a1, a2, a3, a4);
    case SYS_GPU_COMMAND:
        return hmje_posix_gpu_command(a1, a2, a3);
    case SYS_FRAMEBUFFER_INFO:
        return hmje_posix_framebuffer_info(a1, a2);
    case SYS_FRAMEBUFFER_BLIT:
        return hmje_posix_framebuffer_blit(a1, a2, a3, a4, a5, a6);
    case SYS_FRAMEBUFFER_PRESENT:
        return hmje_posix_framebuffer_present(a1, a2);
    case SYS_INPUT_POLL:
        return hmje_posix_input_poll();
    case SYS_INPUT_POLL2:
        return hmje_posix_input_poll2(a1, a2);
    case SYS_UI_SYSTEM_THEME: {
        const char *override = getenv("HMJE_SYSTEM_THEME");
        if (override != NULL && strcmp(override, "dark") == 0) return 2;
        if (override != NULL && strcmp(override, "light") == 0) return 1;
        return hmje_posix_system_theme();
    }
    case SYS_UI_ACCESSIBILITY:
        return hmje_posix_ui_accessibility(a1, a2, a3);
    case SYS_TEXT_INPUT_CONTROL2:
        return hmje_posix_text_input_control2(a1, a2, a3);
    case SYS_TEXT_INPUT_CONTROL:
        return hmje_posix_text_input_control(a1);
    case SYS_UI_WINDOW_CONFIG:
        return hmje_posix_ui_window_config(a1, a2, a3);
    case SYS_UI_CLIPBOARD:
        return hmje_posix_ui_clipboard(a1, a2, a3);
    case SYS_UI_WINDOW:
        return hmje_posix_ui_window(a1, a2, a3);
    case SYS_APP_OPEN_URL:
        return hmje_posix_app_open_url(a1, a2, a3);
    case SYS_UI_DRAG_DROP:
        return hmje_posix_ui_drag_drop(a1, a2, a3);
    case SYS_AUDIO_CAPTURE_OPEN:
        return hmje_posix_audio_capture_open(a1, a2);
    case SYS_AUDIO_CAPTURE_READ:
        return hmje_posix_audio_capture_read(a1, a2, a3, a4, a5);
    case SYS_AUDIO_CAPTURE_CONTROL:
        return hmje_posix_audio_capture_control(a1, a2);
    case SYS_AUDIO_CAPTURE_PERMISSION:
        return hmje_posix_audio_capture_permission(a1);
    case SYS_ARG_SIZES:
        return arg_sizes(a1);
    case SYS_ARG_READ:
        return arg_read(a1, a2);
    case SYS_ENV_READ:
        return env_read(a1, a2, a3, a4);
    default:
        return HMJE_POSIX_ENOSYS;
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
