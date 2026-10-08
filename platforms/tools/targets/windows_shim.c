/*
 * windows_shim.c — hmje 앱을 hosted Windows 프로세스(MSVC ABI)로 돌릴 때의 시스템콜 구현. posix_host_shim.c(macOS)/android_shim.c(Android)와
 * 나란한 시스템콜 경계다.
 *
 * ── 창/프레임버퍼는 windows_window.c(Win32 + D3D11) 소유 ──────────────────────────────
 * 이 파일은 순수 C 시스템콜 디스패치만 담당하고, 프레임버퍼/입력 4개 함수는 extern
 * 선언만 해서 windows_window.c의 강한 정의를 호출한다(posix_host_shim.c ↔
 * posix_host_window.m과 같은 분리 원칙 — Windows는 창 없는 빌드 변형이 없어서 weak 기본값이
 * 필요 없다는 점만 다르다).
 *
 * ── 구현된 syscall (docs/syscall.md 의미 준수, posix_host_shim.c와 동일 계약) ────────────
 *   0  SYS_EXIT                   → exit(code)
 *   1  SYS_WRITE                  → WriteFile(표준출력, ...) — fd 1/2만 지원
 *   2  SYS_TIME_MS / 60 TIME_US   → QueryPerformanceCounter 기반 단조 시계
 *   3  SYS_SLEEP_MS / 61 SLEEP_US → Sleep(ms 단위, us는 반올림)
 *   4  SYS_ALLOC / 6 SYS_FREE     → malloc/free (Windows 힙 주소는 Bionic과 달리 부호
 *                                   있는 i64 비교 문제가 없다 — android_shim.c의 저주소
 *                                   arena 우회가 여기선 불필요)
 *   5  SYS_READ                   → ReadFile(핸들) — 객체 핸들(=진짜 HANDLE)에도 그대로 동작
 *   13 SYS_PROCESS_ID             → GetCurrentProcessId
 *   15 SYS_YIELD                  → SwitchToThread
 *   18/19 SYS_VM_RESERVE/RELEASE  → VirtualAlloc(MEM_RESERVE), 페이지 접근 시 커밋 / VirtualFree(MEM_RELEASE)
 *   20 SYS_OBJECT_OPEN_GRAPH_WALK → "/system/fonts/{name}" → 자원 fonts/{name}(실행파일.assets\fonts\{name})
 *                                   (CreateFileW — macOS/Android와 같은 원칙)
 *   21 SYS_CLOSE                  → CloseHandle
 *   22 SYS_OBJECT_STAT            → GetFileSizeEx를 64바이트 레코드로 번역
 *   37 SYS_OBJECT_READ_AT         → ReadFile + OVERLAPPED(Offset/OffsetHigh) — Windows는
 *                                   pread가 없으므로 OVERLAPPED 포지셔널 read로 흉내낸다.
 *                                   동기 핸들에서는 지정 위치에서 완료될 때까지 기다리는 대신
 *                                   Win32 file pointer도 그 위치 뒤로 갱신된다. HMJE 파일 API는
 *                                   streaming SYS_READ와 혼용하지 않고 READ_AT만 쓰므로 위치
 *                                   지정 계약에는 영향이 없다.
 *   38 SYS_OBJECT_WRITE_AT        → WriteFile + OVERLAPPED 위치지정 쓰기
 *   42 SYS_OBJECT_SYNC            → FlushFileBuffers 영속성 장벽
 *   54/55/62 SYS_FRAMEBUFFER_*    → windows_window.c (Win32 + D3D11)
 *   56 SYS_INPUT_POLL             → windows_window.c
 *   75 SYS_FILE_OPEN              → 앱 전용 데이터 루트 아래 CreateFileW. CREATE_NEW로
 *                                   배타 생성하고 UTF-8 경로를 UTF-16으로 변환한다.
 *   76 SYS_FILE_LIST              → 열린 디렉터리 HANDLE을 FileIdBothDirectoryInfo로
 *                                   열거해 플랫폼 중립 64바이트 레코드로 변환한다.
 *   85/86/87 SYS_SOCKET_*         → Winsock TCP connect/send/recv
 *   118/119 SYS_SOCKET_LISTEN/ACCEPT → 루프백 듣기·수락(windows_socket_server.inc)
 *   91 SYS_FILE_LOCK              → LockFileEx로 EOF에서 멀리 떨어진 예약 sentinel byte를
 *                                   잠근다. 실제 자료 범위와 겹치지 않아 advisory 동작이다.
 *   92 SYS_FILE_UNLINK            → 같은 자료 루트의 일반 파일 pathname을 DeleteFileW로
 *                                   제거한다. 디렉터리는 제거하지 않는다.
 *   94 SYS_FILE_IDENTITY          → 열린 HANDLE의 volume/file-index identity.
 *   99/100 SYS_ARG_SIZES/READ      → Win32 UTF-16 명령줄을 Windows 인용 규칙에
 *                                   따라 분리해 NUL 연접 UTF-8 argv로 제공.
 *
 * 스텁(HMJE_WINDOWS_ENOSYS=-38): 그 외 전부(IPC/부트모듈/디버그/객체 그래프 나머지,
 * UDP) — 다른 hosted 플랫폼과 같은 범위(android_shim.c 참고).
 */


















/* The socket helpers (strcpy on fixed, bounded host buffers) and sscanf-style parsing would
 * otherwise print MSVC CRT deprecation warnings in every build. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
#define SYS_WALL_TIME_MS 114
#define SYS_SECURE_RANDOM_FILL 115
#define SYS_LOCAL_DATETIME 116
#define SYS_INPUT_POLL2 98
#define SYS_ARG_SIZES 99
#define SYS_ARG_READ 100
#define SYS_ENV_READ 101
#define SYS_UI_SYSTEM_THEME 110
#define SYS_TEXT_INPUT_CONTROL 104
#define SYS_TEXT_INPUT_CONTROL2 111
#define SYS_UI_WINDOW_CONFIG 112
#define SYS_UI_CLIPBOARD 113
#define SYS_UI_WINDOW 117
#define SYS_APP_OPEN_URL 120
#define SYS_UI_DRAG_DROP 140

#define HMJE_WINDOWS_ENOSYS (-38LL)

#define FILE_MODE_READ 1
#define FILE_MODE_WRITE 2
#define FILE_MODE_CREATE 4
#define FILE_MODE_EXCLUSIVE_CREATE 8

#define FILE_LOCK_SHARED 1
#define FILE_LOCK_EXCLUSIVE 2
#define FILE_LOCK_UNLOCK 3
#define FILE_LOCK_TRY 4

#define ERR_NOT_FOUND 2
#define ERR_INTERRUPTED 4
#define ERR_IO 5
#define ERR_BAD_HANDLE 9
#define ERR_NO_MEMORY 12
#define ERR_PERMISSION 13
#define ERR_BAD_ADDRESS 14
#define ERR_BUSY 16
#define ERR_ALREADY_EXISTS 17
#define ERR_NOT_DIRECTORY 20
#define ERR_INVALID_ARGUMENT 22
#define ERR_TOO_MANY_FILES 24
#define ERR_NO_SPACE 28
#define ERR_READ_ONLY 30
#define ERR_NAME_TOO_LONG 36
#define ERR_NOT_SUPPORTED 95
#define ERR_SOCKET_NOT_CONNECTED 107
#define ERR_SOCKET_TIMEOUT 110
#define ERR_SOCKET_CONN_REFUSED 111
#define ERR_SOCKET_HOST_NOT_FOUND 113
#define ERR_SOCKET_WOULD_BLOCK 11

#define SOCKET_RECV_NONBLOCK 1
#define SOCKET_DEFAULT_TIMEOUT_MS 10000

#define FILE_LIST_ENTRY_SIZE 64
#define FILE_LIST_NAME_LEN_OFFSET 0
#define FILE_LIST_IS_DIR_OFFSET 8
#define FILE_LIST_SIZE_OFFSET 16
#define FILE_LIST_RESERVED_OFFSET 24
#define FILE_LIST_NAME_OFFSET 32
#define FILE_LIST_NAME_INLINE_MAX 32

typedef long long i64;
typedef unsigned long long u64;

#include "windows_thread_api.inc"


/* Win32 오류를 HMJE/POSIX 호스트와 같은 음수 errno 공간으로 정규화한다. 운영체제의
 * ERROR_* 숫자를 그대로 새 ABI로 누출하지 않는다. */
static i64 win32_error_result(DWORD error) {
    switch (error) {
    case ERROR_SUCCESS:
        return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return -(i64)ERR_NOT_FOUND;
    case ERROR_OPERATION_ABORTED:
        return -(i64)ERR_INTERRUPTED;
    case ERROR_INVALID_HANDLE:
        return -(i64)ERR_BAD_HANDLE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return -(i64)ERR_NO_MEMORY;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return -(i64)ERR_PERMISSION;
    case ERROR_LOCK_VIOLATION:
    case ERROR_SHARING_VIOLATION:
    case ERROR_IO_PENDING:
        return -(i64)ERR_BUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return -(i64)ERR_ALREADY_EXISTS;
    case ERROR_DIRECTORY:
        return -(i64)ERR_NOT_DIRECTORY;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return -(i64)ERR_INVALID_ARGUMENT;
    case ERROR_TOO_MANY_OPEN_FILES:
        return -(i64)ERR_TOO_MANY_FILES;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return -(i64)ERR_NO_SPACE;
    case ERROR_WRITE_PROTECT:
        return -(i64)ERR_READ_ONLY;
    case ERROR_FILENAME_EXCED_RANGE:
        return -(i64)ERR_NAME_TOO_LONG;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return -(i64)ERR_NOT_SUPPORTED;
    default:
        return -(i64)ERR_IO;
    }
}

/* 단일 파일 빌드의 자원 뿌리는 제품 조정기가 생성한 헤더로 전달한다.
 * UNC 경로를 -D 매크로로 전달하면 셸·PowerShell·CRT argv의 인용 처리가
 * 따옴표·백슬래시를 바꿀 수 있으므로 파일 내용으로 전달한다. */


#ifdef __has_include
#if __has_include("hmje_windows_resources_config.h")
#include "hmje_windows_resources_config.h"
#endif
#endif

/* 범용 파일 API의 앱 전용 자료 루트. 실행 위치가 달라도 게스트 로컬 디스크를 쓰도록
 * 기본값은 CWD 기준 상대 경로로 유지한다. 테스트 러너는 C:의 전용 임시 CWD에서 실행한다. */
#ifndef HMJE_WINDOWS_FILE_DATA_DIR
#define HMJE_WINDOWS_FILE_DATA_DIR "host_app_data"
#endif

/* windows_window.c (Win32 + D3D11) 구현. 인자/반환은 hmje_arch_syscall* 규약과 1:1
 * (posix_host_window.m/android_jni.c와 같은 확장자 간 extern 다리 패턴). */
extern i64 hmje_windows_system_theme(void);

extern i64 hmje_windows_framebuffer_info(i64 out_address, i64 out_length);
extern i64 hmje_windows_framebuffer_blit(i64 buffer_address, i64 width, i64 height, i64 pitch, i64 x, i64 y);
extern i64 hmje_windows_framebuffer_present(i64 rects_address, i64 count);
extern i64 hmje_windows_gpu_command(i64 kind, i64 request_address, i64 request_length);
extern i64 hmje_windows_input_poll(void);
extern i64 hmje_windows_input_poll2(i64 out_address, i64 out_length);
extern i64 hmje_windows_text_input_control(i64 request);
extern i64 hmje_windows_text_input_caret(i64 position, i64 size);
extern i64 hmje_windows_ui_window_config(i64 title_address, i64 title_length, i64 flags);
/* 데스크톱 창 관리(SYS_UI_WINDOW 117): windows_window.c(여러 창·창 fiber), headless는 ENOSYS. */
extern i64 hmje_windows_ui_window(i64 operation, i64 argument1, i64 argument2);
extern i64 hmje_windows_ui_drag_drop(i64 operation, i64 address, i64 capacity);
/* 앱을 연 주소(SYS_APP_OPEN_URL 120): windows_window.c(windows_open_url.inc), headless는 ENOSYS. */
extern i64 hmje_windows_app_open_url(i64 operation, i64 address, i64 capacity);

/* ---- 단조 시계: QueryPerformanceCounter (Sleep()은 ms 단위뿐이라 us는 반올림) --------- */

static i64 qpc_frequency_cached = 0;

static i64 qpc_frequency(void) {
    if (qpc_frequency_cached == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpc_frequency_cached = freq.QuadPart;
    }
    return qpc_frequency_cached;
}

static i64 monotonic_ns(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    i64 freq = qpc_frequency();
    if (freq == 0) {
        return 0;
    }
    /* counter*1e9/freq, overflow를 피하려고 두 단계로 나눈다(초 단위 + 나머지). */
    i64 seconds = counter.QuadPart / freq;
    i64 remainder = counter.QuadPart % freq;
    return seconds * 1000000000LL + (remainder * 1000000000LL) / freq;
}

static i64 wall_time_s(void) {
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    /* FILETIME is 100 ns ticks since 1601-01-01 UTC. */
    if (ticks.QuadPart < 116444736000000000ULL) {
        return HMJE_WINDOWS_ENOSYS;
    }
    return (i64)((ticks.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

#include "windows_time_random.inc"

/* ---- SYS_VM_RESERVE/RELEASE: Win32 페이지 접근 시 커밋 -------------------- */
#include "windows_virtual_memory.inc"

/* ---- 제품 자원: 논리 이름 → 실행파일.assets\{이름} ---------------------------------------
 * 표준 자원 API(library/Sources/표준/resource.hmje)의 Windows 원천. 논리 이름의 `/`는
 * `\`로 바꾼다. 프로젝트 그래프가 없는 단일 파일 개발 빌드는 제품 조정기가
 * hmje_windows_resources_config.h에 HMJE_WINDOWS_RESOURCES_DIR(HMJE_UI_ROOT\assets)을 박는다.
 * 옛 `/system/fonts/{name}` graph walk는 `fonts/{name}` 자원의 별칭이다. */
static int windows_resource_name_valid(const char *name, size_t name_len) {
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

static i64 open_resource_file(const char *name, size_t name_len) {
    WCHAR full[32768];
    size_t capacity = sizeof(full) / sizeof(full[0]);
    size_t prefix_length = 0;
#ifdef HMJE_WINDOWS_RESOURCES_DIR
    int root_length = MultiByteToWideChar(CP_UTF8, 0, HMJE_WINDOWS_RESOURCES_DIR, -1, full, (int)capacity);
    if (root_length <= 1) {
        return -(i64)ERR_NOT_FOUND;
    }
    prefix_length = (size_t)root_length - 1;
    if (prefix_length + 1 >= capacity) {
        return -(i64)ERR_NOT_FOUND;
    }
    full[prefix_length++] = L'\\';
#else
    DWORD executable_length = GetModuleFileNameW(NULL, full, (DWORD)capacity);
    if (executable_length == 0 || executable_length >= (DWORD)capacity) {
        return -(i64)ERR_NOT_FOUND;
    }
    WCHAR *filename = full;
    for (WCHAR *cursor = full; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            filename = cursor + 1;
        }
    }
    WCHAR *extension = NULL;
    for (WCHAR *cursor = filename; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'.') {
            extension = cursor;
        }
    }
    if (extension != NULL && _wcsicmp(extension, L".exe") == 0) {
        *extension = L'\0';
        executable_length = (DWORD)(extension - full);
    }
    static const WCHAR suffix[] = L".assets\\";
    size_t suffix_length = (sizeof(suffix) / sizeof(suffix[0])) - 1;
    if ((size_t)executable_length + suffix_length + 1 >= capacity) {
        return -(i64)ERR_NOT_FOUND;
    }
    memcpy(full + executable_length, suffix, suffix_length * sizeof(WCHAR));
    prefix_length = (size_t)executable_length + suffix_length;
#endif
    int name_wide_length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)name_len, NULL, 0
    );
    if (name_wide_length <= 0 || prefix_length + (size_t)name_wide_length + 1 > capacity) {
        return -(i64)ERR_NOT_FOUND;
    }
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            name,
            (int)name_len,
            full + prefix_length,
            name_wide_length
        ) != name_wide_length) {
        return -(i64)ERR_NOT_FOUND;
    }
    for (size_t i = prefix_length; i < prefix_length + (size_t)name_wide_length; ++i) {
        if (full[i] == L'/') {
            full[i] = L'\\';
        }
    }
    full[prefix_length + (size_t)name_wide_length] = L'\0';
    HANDLE handle = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_NOT_FOUND;
    }
    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(handle);
        return -(i64)ERR_NOT_FOUND;
    }
    return (i64)(uintptr_t)handle;
}

/* 표준 자원 API의 hosted 원천(library/Sources/표준호스트/resource). */
i64 hmje_resource_open(const char *name, u64 name_len) {
    if (!windows_resource_name_valid(name, (size_t)name_len)) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    return open_resource_file(name, (size_t)name_len);
}

static i64 object_open_graph_walk(i64 path_addr, i64 path_len) {
    static const char prefix[] = "/system/fonts/";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (path_len <= 0 || (u64)path_len <= prefix_len) {
        return HMJE_WINDOWS_ENOSYS;
    }
    const char *path_bytes = (const char *)(uintptr_t)(u64)path_addr;
    if (memcmp(path_bytes, prefix, prefix_len) != 0) {
        return HMJE_WINDOWS_ENOSYS;
    }
    /* "/system/fonts/{rel}" → 자원 "fonts/{rel}". */
    const char *name = path_bytes + sizeof("/system/") - 1;
    size_t name_len = (size_t)path_len - (sizeof("/system/") - 1);
    if (name_len > 1024 || !windows_resource_name_valid(name, name_len)) {
        return HMJE_WINDOWS_ENOSYS;
    }
    i64 handle = open_resource_file(name, name_len);
    return handle > 0 ? handle : HMJE_WINDOWS_ENOSYS;
}

/* ---- SYS_OBJECT_STAT / SYS_OBJECT_READ_AT / SYS_CLOSE: 진짜 HANDLE 기준 -------------- */

static i64 object_stat(i64 handle, i64 out_addr, i64 out_len) {
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (out_addr == 0 || out_len < 64) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)(uintptr_t)handle, &size)) {
        return win32_error_result(GetLastError());
    }
    u64 record[8];
    memset(record, 0, sizeof(record));
    record[0] = (u64)handle;
    record[3] = (u64)size.QuadPart;
    memcpy((void *)(uintptr_t)(u64)out_addr, record, sizeof(record));
    return 0;
}

static i64 file_identity(i64 handle, i64 out_addr, i64 out_length) {
    BY_HANDLE_FILE_INFORMATION information;
    u64 identity[2];
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (out_addr == 0 || out_length < (i64)sizeof(identity)) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    if (!GetFileInformationByHandle(
            (HANDLE)(uintptr_t)handle,
            &information
        )) {
        return win32_error_result(GetLastError());
    }
    identity[0] = (u64)information.dwVolumeSerialNumber;
    identity[1] = ((u64)information.nFileIndexHigh << 32) |
        (u64)information.nFileIndexLow;
    memcpy((void *)(uintptr_t)(u64)out_addr, identity, sizeof(identity));
    return 0;
}

/* SYS_OBJECT_READ_AT(37): OVERLAPPED의 Offset/OffsetHigh로 위치 지정 read. 동기 HANDLE은
 * 지정 위치에서 완료될 때까지 기다리고 file pointer도 갱신한다. 범용 파일 API는 같은
 * HANDLE의 streaming SYS_READ와 혼용하지 않는다는 Windows hosted 전제가 있다. */
static i64 object_read_at(i64 handle, i64 offset, i64 out_addr, i64 length, i64 flags, i64 reserved) {
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (offset < 0 || length < 0 || (length != 0 && out_addr == 0) ||
        flags != 0 || reserved != 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    if ((u64)length > (u64)MAXDWORD) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)((u64)offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = (DWORD)((u64)offset >> 32);
    DWORD read_bytes = 0;
    BOOL ok = ReadFile((HANDLE)(uintptr_t)handle, (LPVOID)(uintptr_t)(u64)out_addr,
                        (DWORD)length, &read_bytes, &overlapped);
    if (!ok) {
        /* ERROR_HANDLE_EOF는 "0바이트 읽음"으로, 다른 실패와 구분해 정상 반환한다. */
        if (GetLastError() == ERROR_HANDLE_EOF) {
            return 0;
        }
        return win32_error_result(GetLastError());
    }
    return (i64)read_bytes;
}

/* SYS_OBJECT_WRITE_AT(38): 동기 HANDLE + OVERLAPPED offset 조합은 해당 위치에서 작업이
 * 끝날 때까지 반환하지 않는다. FILE_FLAG_OVERLAPPED를 쓰지 않으므로 별도 event 수명이나
 * ERROR_IO_PENDING 처리도 필요 없다. */
static i64 object_write_at(i64 handle, i64 offset, i64 in_addr, i64 length, i64 flags, i64 reserved) {
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (offset < 0 || length < 0 || (length != 0 && in_addr == 0) ||
        flags != 0 || reserved != 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    if ((u64)length > (u64)MAXDWORD) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)((u64)offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = (DWORD)((u64)offset >> 32);
    DWORD written_bytes = 0;
    BOOL ok = WriteFile((HANDLE)(uintptr_t)handle, (LPCVOID)(uintptr_t)(u64)in_addr,
                        (DWORD)length, &written_bytes, &overlapped);
    return ok ? (i64)written_bytes : win32_error_result(GetLastError());
}

/* SYS_OBJECT_SYNC(42): Windows의 명시적 file durability barrier. */
static i64 object_sync(i64 handle) {
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    return FlushFileBuffers((HANDLE)(uintptr_t)handle) ? 0 : win32_error_result(GetLastError());
}

/* ---- SYS_FILE_OPEN / SYS_FILE_LOCK / SYS_FILE_UNLINK -------------------------------- */

#define HMJE_WINDOWS_PATH_CAP 4160

static int is_windows_forbidden_ascii(unsigned char byte) {
    return byte < 32 || byte == '<' || byte == '>' || byte == ':' || byte == '"' ||
           byte == '\\' || byte == '|' || byte == '?' || byte == '*';
}

static unsigned char ascii_upper(unsigned char byte) {
    return byte >= 'a' && byte <= 'z' ? (unsigned char)(byte - ('a' - 'A')) : byte;
}

/* CreateFileW가 일반 파일 대신 DOS device namespace로 해석하는 이름을 component 단위로
 * 막는다. 확장자가 붙어도(CON.txt 등) device이므로 첫 '.' 앞까지만 비교한다. */
static int is_windows_reserved_component(const unsigned char *bytes, size_t length) {
    size_t base_length = 0;
    while (base_length < length && bytes[base_length] != '.') {
        ++base_length;
    }
    if (base_length == 3) {
        unsigned char a = ascii_upper(bytes[0]);
        unsigned char b = ascii_upper(bytes[1]);
        unsigned char c = ascii_upper(bytes[2]);
        return (a == 'C' && b == 'O' && c == 'N') ||
               (a == 'P' && b == 'R' && c == 'N') ||
               (a == 'A' && b == 'U' && c == 'X') ||
               (a == 'N' && b == 'U' && c == 'L');
    }
    if (base_length == 4) {
        unsigned char a = ascii_upper(bytes[0]);
        unsigned char b = ascii_upper(bytes[1]);
        unsigned char c = ascii_upper(bytes[2]);
        unsigned char d = bytes[3];
        return d >= '0' && d <= '9' &&
               ((a == 'C' && b == 'O' && c == 'M') ||
                (a == 'L' && b == 'P' && c == 'T'));
    }
    /* Win32는 ISO-8859-1의 ¹, ², ³도 ASCII 숫자와 같은 DOS device
     * suffix로 취급한다. UTF-8(C2 B9/B2/B3)인 경우도 namespace로
     * 빠지기 전에 거부한다. */
    if (base_length == 5) {
        unsigned char a = ascii_upper(bytes[0]);
        unsigned char b = ascii_upper(bytes[1]);
        unsigned char c = ascii_upper(bytes[2]);
        unsigned char d = bytes[3];
        unsigned char e = bytes[4];
        return d == 0xc2 && (e == 0xb9 || e == 0xb2 || e == 0xb3) &&
               ((a == 'C' && b == 'O' && c == 'M') ||
                (a == 'L' && b == 'P' && c == 'T'));
    }
    if (base_length == 6) {
        return ascii_upper(bytes[0]) == 'C' &&
               ascii_upper(bytes[1]) == 'O' &&
               ascii_upper(bytes[2]) == 'N' &&
               ascii_upper(bytes[3]) == 'I' &&
               ascii_upper(bytes[4]) == 'N' &&
               bytes[5] == '$';
    }
    if (base_length == 7) {
        return ascii_upper(bytes[0]) == 'C' &&
               ascii_upper(bytes[1]) == 'O' &&
               ascii_upper(bytes[2]) == 'N' &&
               ascii_upper(bytes[3]) == 'O' &&
               ascii_upper(bytes[4]) == 'U' &&
               ascii_upper(bytes[5]) == 'T' &&
               bytes[6] == '$';
    }
    return 0;
}

/* HMJE 절대 표기("/a/b")를 UTF-8로 검증한 뒤 앱 자료 루트 아래 UTF-16 경로로 만든다.
 * Windows에서 별도 separator인 '\\'와 ADS/device 문법을 여는 ':' 등도 거부한다. */
static i64 file_resolve_path(i64 path_addr, i64 path_len, WCHAR *full, size_t full_cap) {
#if defined(HMJE_COMPILER_HOST_FILES)
    /* Only compiler executables opt in. User applications keep the sandboxed
     * resolver below; the compiler must read projects and write its cache. */
    if (path_len <= 0 || path_len >= 4096 || path_addr == 0 || full_cap < 2)
        return -(i64)ERR_INVALID_ARGUMENT;
    const char *bytes = (const char *)(uintptr_t)(u64)path_addr;
    if (memchr(bytes, '\0', (size_t)path_len)) return -(i64)ERR_INVALID_ARGUMENT;
    WCHAR input[HMJE_WINDOWS_PATH_CAP];
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                     (int)path_len, input, HMJE_WINDOWS_PATH_CAP - 1);
    if (length <= 0) return -(i64)ERR_INVALID_ARGUMENT;
    input[length] = L'\0';
    for (int i = 0; i < length; ++i) if (input[i] == L'/') input[i] = L'\\';
    DWORD written = GetFullPathNameW(input, (DWORD)full_cap, full, NULL);
    if (written == 0 || written >= full_cap) return -(i64)ERR_NAME_TOO_LONG;
    return 0;
#else
    if (path_len <= 1 || path_len >= 4096 || path_addr == 0 || full_cap < 2) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    const unsigned char *bytes = (const unsigned char *)(uintptr_t)(u64)path_addr;
    size_t byte_count = (size_t)path_len;
    if (bytes[0] != '/' || memchr(bytes, '\0', byte_count) != NULL) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }

    size_t component_start = 1;
    for (size_t i = 1; i <= byte_count; ++i) {
        if (i != byte_count && bytes[i] != '/') {
            if (is_windows_forbidden_ascii(bytes[i])) {
                return -(i64)ERR_INVALID_ARGUMENT;
            }
            continue;
        }
        size_t component_len = i - component_start;
        if (component_len == 0 ||
            (component_len == 1 && bytes[component_start] == '.') ||
            (component_len == 2 && bytes[component_start] == '.' && bytes[component_start + 1] == '.') ||
            is_windows_reserved_component(bytes + component_start, component_len) ||
            bytes[i - 1] == '.' || bytes[i - 1] == ' ') {
            return -(i64)ERR_INVALID_ARGUMENT;
        }
        component_start = i + 1;
    }

    int root_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       HMJE_WINDOWS_FILE_DATA_DIR, -1,
                                       full, (int)full_cap);
    if (root_len <= 0) {
        return win32_error_result(GetLastError());
    }
    size_t used = (size_t)root_len - 1;
    if (used + 2 >= full_cap) {
        return -(i64)ERR_NAME_TOO_LONG;
    }
    if (used != 0 && full[used - 1] != L'\\' && full[used - 1] != L'/') {
        full[used++] = L'\\';
    }

    int relative_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           (const char *)bytes + 1, (int)byte_count - 1,
                                           full + used, (int)(full_cap - used - 1));
    if (relative_len <= 0) {
        return win32_error_result(GetLastError());
    }
    for (int i = 0; i < relative_len; ++i) {
        if (full[used + (size_t)i] == L'/') {
            full[used + (size_t)i] = L'\\';
        }
    }
    full[used + (size_t)relative_len] = L'\0';
    return 0;
#endif
}

static int path_is_directory(const WCHAR *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static i64 ensure_one_directory(WCHAR *path) {
    if (CreateDirectoryW(path, NULL)) {
        return 0;
    }
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS && path_is_directory(path)) {
        return 0;
    }
    return win32_error_result(error);
}

/* file 경로의 parent를 mkdir -p 한다. UNC에서는 \\server\share prefix를 생성 대상으로
 * 보지 않고, drive 경로에서는 "C:\\" 뒤부터 시작한다. */
static i64 file_ensure_parent_dirs(const WCHAR *full) {
    WCHAR parent[HMJE_WINDOWS_PATH_CAP];
    size_t length = wcslen(full);
    if (length >= HMJE_WINDOWS_PATH_CAP) {
        return -(i64)ERR_NAME_TOO_LONG;
    }
    memcpy(parent, full, (length + 1) * sizeof(WCHAR));
    WCHAR *last_slash = wcsrchr(parent, L'\\');
    if (last_slash == NULL) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    *last_slash = L'\0';

    size_t start = 0;
    if (parent[0] == L'\\' && parent[1] == L'\\') {
        WCHAR *server_end = wcschr(parent + 2, L'\\');
        WCHAR *share_end = server_end == NULL ? NULL : wcschr(server_end + 1, L'\\');
        if (server_end == NULL) {
            return -(i64)ERR_INVALID_ARGUMENT;
        }
        start = share_end == NULL ? wcslen(parent) : (size_t)(share_end - parent + 1);
    } else if (((parent[0] >= L'A' && parent[0] <= L'Z') ||
                (parent[0] >= L'a' && parent[0] <= L'z')) &&
               parent[1] == L':' && parent[2] == L'\\') {
        start = 3;
    }

    for (size_t i = start; parent[i] != L'\0'; ++i) {
        if (parent[i] != L'\\') {
            continue;
        }
        parent[i] = L'\0';
        i64 result = ensure_one_directory(parent);
        parent[i] = L'\\';
        if (result != 0) {
            return result;
        }
    }
    return ensure_one_directory(parent);
}

static BOOL file_flush_exclusive_create(HANDLE handle) {
#ifdef HMJE_WINDOWS_TEST_EXCLUSIVE_SYNC_ERROR
    /* Native harness 전용 failure injection. 일반 빌드에는 매크로가 없으므로 이 분기는
     * 존재하지 않으며, cleanup이 최초 Flush 오류를 보존하는 경계를 실기로 검증한다. */
    (void)handle;
    SetLastError((DWORD)HMJE_WINDOWS_TEST_EXCLUSIVE_SYNC_ERROR);
    return FALSE;
#else
    return FlushFileBuffers(handle);
#endif
}

/* CREATE_NEW 뒤 durability 단계를 끝내지 못한 경우에만 호출한다. HANDLE을 먼저 닫아
 * DeleteFileW의 sharing 조건을 만족시키고, 정리 결과는 버려 최초 오류를 보존한다. */
static void file_cleanup_failed_exclusive_create(HANDLE handle, const WCHAR *full) {
    (void)CloseHandle(handle);
    (void)DeleteFileW(full);
}

static i64 file_open(i64 path_addr, i64 path_len, i64 mode) {
    if ((mode & ~(FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE_CREATE)) != 0 ||
        (mode & (FILE_MODE_READ | FILE_MODE_WRITE)) == 0 ||
        ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0 && (mode & FILE_MODE_CREATE) == 0)) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }

    WCHAR full[HMJE_WINDOWS_PATH_CAP];
    i64 resolve_result = file_resolve_path(path_addr, path_len, full, HMJE_WINDOWS_PATH_CAP);
    if (resolve_result != 0) {
        return resolve_result;
    }
    if ((mode & FILE_MODE_CREATE) != 0) {
        i64 directory_result = file_ensure_parent_dirs(full);
        if (directory_result != 0) {
            return directory_result;
        }
    }

    DWORD access = GENERIC_READ;
    if ((mode & FILE_MODE_WRITE) != 0) {
        access |= GENERIC_WRITE;
    }
    DWORD disposition = OPEN_EXISTING;
    if ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0) {
        disposition = CREATE_NEW;
    } else if ((mode & FILE_MODE_CREATE) != 0) {
        disposition = OPEN_ALWAYS;
    }
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = CreateFileW(full, access, share, NULL, disposition,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS |
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return win32_error_result(GetLastError());
    }

    /* CREATE_NEW 성공은 기존 파일과 구별되는 초기화 경계다. 반환 전에 Win32가 제공하는
     * 가장 강한 파일 handle barrier를 수행한다. 읽기전용 생성이면 잠시 쓰기 handle을
     * 따로 열어, 호출자에게 반환하는 권한은 넓히지 않는다. */
    if ((mode & FILE_MODE_EXCLUSIVE_CREATE) != 0) {
        HANDLE sync_handle = handle;
        if ((mode & FILE_MODE_WRITE) == 0) {
            sync_handle = CreateFileW(full, GENERIC_WRITE, share, NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
            if (sync_handle == INVALID_HANDLE_VALUE) {
                i64 result = win32_error_result(GetLastError());
                file_cleanup_failed_exclusive_create(handle, full);
                return result;
            }
        }
        if (!file_flush_exclusive_create(sync_handle)) {
            i64 result = win32_error_result(GetLastError());
            if (sync_handle != handle) {
                (void)CloseHandle(sync_handle);
            }
            file_cleanup_failed_exclusive_create(handle, full);
            return result;
        }
        if (sync_handle != handle) {
            CloseHandle(sync_handle);
        }
    }
    return (i64)(uintptr_t)handle;
}

static void file_list_store_u64(unsigned char *record, size_t offset, u64 value) {
    memcpy(record + offset, &value, sizeof(value));
}

static int file_list_is_dot_entry(const FILE_ID_BOTH_DIR_INFO *entry) {
    DWORD one = (DWORD)sizeof(WCHAR);
    DWORD two = 2u * (DWORD)sizeof(WCHAR);
    return (entry->FileNameLength == one && entry->FileName[0] == L'.') ||
           (entry->FileNameLength == two && entry->FileName[0] == L'.' &&
            entry->FileName[1] == L'.');
}

/* SYS_FILE_LIST(76): pathname을 다시 해석하지 않고 열린 디렉터리 HANDLE 자체를
 * 열거한다. 한 호출은 항상 첫 항목부터 최대 max_count개를 반환한다. API에 cursor가
 * 없고 SemtleOS backend도 같은 snapshot-prefix 계약이므로 RestartInfo를 사용한다. */
static i64 file_list(i64 handle, i64 out_addr, i64 max_count) {
    HANDLE directory = (HANDLE)(uintptr_t)handle;
    if (handle == 0 || directory == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (max_count < 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    if (max_count == 0) {
        return 0;
    }
    if (out_addr == 0) {
        return -(i64)ERR_BAD_ADDRESS;
    }

    FILE_STANDARD_INFO standard;
    if (!GetFileInformationByHandleEx(directory, FileStandardInfo,
                                      &standard, (DWORD)sizeof(standard))) {
        return win32_error_result(GetLastError());
    }
    if (!standard.Directory) {
        return -(i64)ERR_NOT_DIRECTORY;
    }

    const DWORD query_size = 64u * 1024u;
    unsigned char *query = (unsigned char *)malloc(query_size);
    if (query == NULL) {
        return -(i64)ERR_NO_MEMORY;
    }

    unsigned char *output = (unsigned char *)(uintptr_t)(u64)out_addr;
    i64 count = 0;
    FILE_INFO_BY_HANDLE_CLASS query_class = FileIdBothDirectoryRestartInfo;
    while (count < max_count) {
        if (!GetFileInformationByHandleEx(directory, query_class, query, query_size)) {
            DWORD error = GetLastError();
            free(query);
            return error == ERROR_NO_MORE_FILES ? count : win32_error_result(error);
        }
        query_class = FileIdBothDirectoryInfo;

        FILE_ID_BOTH_DIR_INFO *entry = (FILE_ID_BOTH_DIR_INFO *)(void *)query;
        for (;;) {
            if (!file_list_is_dot_entry(entry)) {
                if ((entry->FileNameLength % sizeof(WCHAR)) != 0) {
                    free(query);
                    return -(i64)ERR_IO;
                }
                int wide_length = (int)(entry->FileNameLength / sizeof(WCHAR));
                int utf8_length = WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName, wide_length,
                    NULL, 0, NULL, NULL
                );
                if (utf8_length <= 0) {
                    i64 result = win32_error_result(GetLastError());
                    free(query);
                    return result;
                }
                char *utf8_name = (char *)malloc((size_t)utf8_length);
                if (utf8_name == NULL) {
                    free(query);
                    return -(i64)ERR_NO_MEMORY;
                }
                int converted = WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName, wide_length,
                    utf8_name, utf8_length, NULL, NULL
                );
                if (converted != utf8_length) {
                    i64 result = converted <= 0
                        ? win32_error_result(GetLastError())
                        : -(i64)ERR_IO;
                    free(utf8_name);
                    free(query);
                    return result;
                }

                unsigned char *record =
                    output + (size_t)count * FILE_LIST_ENTRY_SIZE;
                size_t copy_length = (size_t)utf8_length;
                if (copy_length > FILE_LIST_NAME_INLINE_MAX) {
                    copy_length = FILE_LIST_NAME_INLINE_MAX;
                }
                file_list_store_u64(record, FILE_LIST_NAME_LEN_OFFSET,
                                    (u64)(unsigned int)utf8_length);
                file_list_store_u64(
                    record, FILE_LIST_IS_DIR_OFFSET,
                    (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 1 : 0
                );
                /* +16은 모든 port에서 예약 필드다. 크기는 SYS_OBJECT_STAT으로 읽는다. */
                file_list_store_u64(record, FILE_LIST_SIZE_OFFSET, 0);
                file_list_store_u64(record, FILE_LIST_RESERVED_OFFSET, 0);
                memset(record + FILE_LIST_NAME_OFFSET, 0, FILE_LIST_NAME_INLINE_MAX);
                memcpy(record + FILE_LIST_NAME_OFFSET, utf8_name, copy_length);
                free(utf8_name);
                ++count;
                if (count >= max_count) {
                    break;
                }
            }

            if (entry->NextEntryOffset == 0) {
                break;
            }
            entry = (FILE_ID_BOTH_DIR_INFO *)(void *)(
                (unsigned char *)(void *)entry + entry->NextEntryOffset
            );
        }
    }

    free(query);
    return count;
}

/* SYS_FILE_UNLINK(92): open과 같은 UTF-8/namespace validator를 재사용하고 일반 파일
 * 삭제 전용 DeleteFileW만 호출한다. RemoveDirectoryW로 폴백하지 않으므로 디렉터리는
 * 이 syscall로 제거할 수 없다. Win32에는 parent directory fsync 공개 계약이 없다. */
static i64 file_unlink(i64 path_addr, i64 path_len, i64 reserved) {
    if (reserved != 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }

    WCHAR full[HMJE_WINDOWS_PATH_CAP];
    i64 resolve_result = file_resolve_path(path_addr, path_len, full, HMJE_WINDOWS_PATH_CAP);
    if (resolve_result != 0) {
        return resolve_result;
    }
    return DeleteFileW(full) ? 0 : win32_error_result(GetLastError());
}

/* 실제 DB 자료 I/O를 막지 않는 advisory coordination byte. pager는 마지막 4KiB
 * (0x7fff_ffff_ffff_f000 이상)를 예약한다. EOF 너머 byte-range lock은 Win32가 허용한다.
 * LockFileEx는 같은 HANDLE의 겹친 lock을 누적하므로 mode를 바꾸거나 다시 잠그기 전에
 * 반드시 FILE_LOCK_UNLOCK을 호출한다. 엔진은 begin/open 경계에서 이 순서를 지킨다. */
#define FILE_LOCK_SENTINEL_LOW 0xFFFFF000u
#define FILE_LOCK_SENTINEL_HIGH 0x7FFFFFFFu

static i64 file_lock(i64 handle, i64 mode, i64 reserved) {
    if (handle == 0 || (HANDLE)(uintptr_t)handle == INVALID_HANDLE_VALUE) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (reserved != 0 || (mode & ~(i64)7) != 0) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    i64 operation = mode & 3;
    if (operation != FILE_LOCK_SHARED && operation != FILE_LOCK_EXCLUSIVE &&
        operation != FILE_LOCK_UNLOCK) {
        return -(i64)ERR_INVALID_ARGUMENT;
    }

    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = FILE_LOCK_SENTINEL_LOW;
    overlapped.OffsetHigh = FILE_LOCK_SENTINEL_HIGH;

    if (operation == FILE_LOCK_UNLOCK) {
        if (UnlockFileEx((HANDLE)(uintptr_t)handle, 0, 1, 0, &overlapped)) {
            return 0;
        }
        /* flock(LOCK_UN)처럼 이미 해제된 핸들의 unlock도 멱등 성공으로 본다. */
        if (GetLastError() == ERROR_NOT_LOCKED) {
            return 0;
        }
        return win32_error_result(GetLastError());
    }

    DWORD flags = operation == FILE_LOCK_EXCLUSIVE ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if ((mode & FILE_LOCK_TRY) != 0) {
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    }
    if (LockFileEx((HANDLE)(uintptr_t)handle, flags, 0, 1, 0, &overlapped)) {
        return 0;
    }
    return win32_error_result(GetLastError());
}

/* ---- SYS_SOCKET_CONNECT/SEND/RECV: Winsock TCP client ------------------------------ */

static INIT_ONCE winsock_init_once = INIT_ONCE_STATIC_INIT;
static int winsock_init_status = WSANOTINITIALISED;

static BOOL CALLBACK winsock_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    WSADATA data;
    (void)once;
    (void)parameter;
    (void)context;
    winsock_init_status = WSAStartup(MAKEWORD(2, 2), &data);
    return TRUE;
}

static int ensure_winsock(void) {
    if (!InitOnceExecuteOnce(&winsock_init_once, winsock_initialize, NULL, NULL)) {
        return 0;
    }
    return winsock_init_status == 0;
}

enum windows_socket_connect_failure {
    WINDOWS_SOCKET_CONNECT_OTHER = 0,
    WINDOWS_SOCKET_CONNECT_REFUSED,
    WINDOWS_SOCKET_CONNECT_TIMEOUT,
};

static i64 windows_socket_connect(i64 host_addr, i64 host_len, i64 port, i64 timeout_ms) {
    if (host_addr == 0 || host_len <= 0 || host_len >= 256 || port <= 0 || port > 65535 ||
        !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    const char *host_bytes = (const char *)(uintptr_t)(u64)host_addr;
    if (memchr(host_bytes, '\0', (size_t)host_len) != NULL) {
        return HMJE_WINDOWS_ENOSYS;
    }
    char host[256];
    memcpy(host, host_bytes, (size_t)host_len);
    host[host_len] = '\0';

    char port_text[16];
    _snprintf_s(port_text, sizeof(port_text), _TRUNCATE, "%lld", (long long)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0 || addresses == NULL) {
        return -(i64)ERR_SOCKET_HOST_NOT_FOUND;
    }

    i64 effective_timeout = timeout_ms > 0 ? timeout_ms : SOCKET_DEFAULT_TIMEOUT_MS;
    enum windows_socket_connect_failure last_failure = WINDOWS_SOCKET_CONNECT_OTHER;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        SOCKET sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
            closesocket(sock);
            continue;
        }

        int result = connect(sock, address->ai_addr, (int)address->ai_addrlen);
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL) {
                last_failure = error == WSAECONNREFUSED
                    ? WINDOWS_SOCKET_CONNECT_REFUSED
                    : WINDOWS_SOCKET_CONNECT_OTHER;
                closesocket(sock);
                continue;
            }

            fd_set writable;
            fd_set exceptional;
            FD_ZERO(&writable);
            FD_ZERO(&exceptional);
            FD_SET(sock, &writable);
            FD_SET(sock, &exceptional);
            struct timeval wait;
            wait.tv_sec = (long)(effective_timeout / 1000);
            wait.tv_usec = (long)((effective_timeout % 1000) * 1000);
            int selected = select(0, NULL, &writable, &exceptional, &wait);
            if (selected == 0) {
                last_failure = WINDOWS_SOCKET_CONNECT_TIMEOUT;
                closesocket(sock);
                continue;
            }
            if (selected == SOCKET_ERROR) {
                last_failure = WINDOWS_SOCKET_CONNECT_OTHER;
                closesocket(sock);
                continue;
            }

            int socket_error = 0;
            int socket_error_length = (int)sizeof(socket_error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&socket_error,
                           &socket_error_length) != 0 || socket_error != 0) {
                last_failure = socket_error == WSAECONNREFUSED
                    ? WINDOWS_SOCKET_CONNECT_REFUSED
                    : WINDOWS_SOCKET_CONNECT_OTHER;
                closesocket(sock);
                continue;
            }
        }

        u_long blocking = 0;
        if (ioctlsocket(sock, FIONBIO, &blocking) != 0) {
            closesocket(sock);
            continue;
        }
        freeaddrinfo(addresses);
        return (i64)(u64)(uintptr_t)sock;
    }

    freeaddrinfo(addresses);
    if (last_failure == WINDOWS_SOCKET_CONNECT_REFUSED) {
        return -(i64)ERR_SOCKET_CONN_REFUSED;
    }
    if (last_failure == WINDOWS_SOCKET_CONNECT_TIMEOUT) {
        return -(i64)ERR_SOCKET_TIMEOUT;
    }
    return HMJE_WINDOWS_ENOSYS;
}

static i64 windows_socket_send(i64 handle, i64 in_addr, i64 length) {
    if (handle <= 0 || length < 0 || (length != 0 && in_addr == 0) || !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    if (length == 0) {
        return 0;
    }
    int amount = length > INT_MAX ? INT_MAX : (int)length;
    int written = send((SOCKET)(uintptr_t)(u64)handle,
                       (const char *)(uintptr_t)(u64)in_addr, amount, 0);
    if (written == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAENOTCONN || error == WSAECONNRESET || error == WSAESHUTDOWN) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_WINDOWS_ENOSYS;
    }
    return (i64)written;
}

static i64 windows_socket_recv(i64 handle, i64 out_addr, i64 length, i64 flags) {
    if (handle <= 0 || length < 0 || (length != 0 && out_addr == 0) ||
        (flags & ~SOCKET_RECV_NONBLOCK) != 0 || !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    if (length == 0) {
        return 0;
    }
    int amount = length > INT_MAX ? INT_MAX : (int)length;
    if ((flags & SOCKET_RECV_NONBLOCK) != 0) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET((SOCKET)(uintptr_t)(u64)handle, &readable);
        struct timeval no_wait;
        no_wait.tv_sec = 0;
        no_wait.tv_usec = 0;
        int selected = select(0, &readable, NULL, NULL, &no_wait);
        if (selected == SOCKET_ERROR) {
            return HMJE_WINDOWS_ENOSYS;
        }
        if (selected == 0) {
            return -(i64)ERR_SOCKET_WOULD_BLOCK;
        }
    }
    int received = recv((SOCKET)(uintptr_t)(u64)handle,
                        (char *)(uintptr_t)(u64)out_addr, amount, 0);
    if (received == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return -(i64)ERR_SOCKET_WOULD_BLOCK;
        }
        if (error == WSAENOTCONN || error == WSAECONNRESET || error == WSAESHUTDOWN) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_WINDOWS_ENOSYS;
    }
    return (i64)received;
}

/* ---- SYS_SOCKET_LISTEN/ACCEPT, 비차단 송신, 비동기 준비 대기: 듣기(서버) 소켓 ---------- */
#include "windows_socket_server.inc"

static i64 windows_close_handle_or_socket(i64 handle) {
    if (handle == 0) {
        return -(i64)ERR_BAD_HANDLE;
    }
    if (ensure_winsock()) {
        SOCKET sock = (SOCKET)(uintptr_t)(u64)handle;
        int socket_type = 0;
        int socket_type_length = (int)sizeof(socket_type);
        if (getsockopt(sock, SOL_SOCKET, SO_TYPE, (char *)&socket_type, &socket_type_length) == 0) {
            return closesocket(sock) == 0 ? 0 : HMJE_WINDOWS_ENOSYS;
        }
    }
    return CloseHandle((HANDLE)(uintptr_t)(u64)handle)
        ? 0
        : win32_error_result(GetLastError());
}

static i64 windows_ui_clipboard(i64 operation, i64 address, i64 length) {
    if (operation == 0) {
        if (!OpenClipboard(NULL)) return -(i64)ERR_BUSY;
        HANDLE value = GetClipboardData(CF_UNICODETEXT);
        if (value == NULL) { CloseClipboard(); return 0; }
        const WCHAR *wide = (const WCHAR *)GlobalLock(value);
        if (wide == NULL) { CloseClipboard(); return -(i64)ERR_IO; }
        size_t wide_length = wcslen(wide);
        if (wide_length > INT_MAX) {
            GlobalUnlock(value);
            CloseClipboard();
            return -(i64)ERR_IO;
        }
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)wide_length,
                                        NULL, 0, NULL, NULL);
        i64 result = bytes == 0 && wide_length != 0 ? -(i64)ERR_IO : (i64)bytes;
        if (result >= 0 && address != 0 && length >= result && result > 0) {
            int copied = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)wide_length,
                                             (char *)(uintptr_t)(u64)address,
                                             (int)result, NULL, NULL);
            if (copied != (int)result) result = -(i64)ERR_IO;
        }
        GlobalUnlock(value);
        CloseClipboard();
        return result;
    }
    if (operation == 1) {
        if (length < 0 || length > INT_MAX || (address == 0 && length != 0)) return -(i64)ERR_BAD_ADDRESS;
        int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             (const char *)(uintptr_t)(u64)address,
                                             (int)length, NULL, 0);
        if (wide_count == 0 && length != 0) return -(i64)ERR_IO;
        SIZE_T bytes = ((SIZE_T)wide_count + 1u) * sizeof(WCHAR);
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (block == NULL) return -(i64)ERR_NO_MEMORY;
        WCHAR *wide = (WCHAR *)GlobalLock(block);
        if (wide == NULL) { GlobalFree(block); return -(i64)ERR_NO_MEMORY; }
        if (wide_count > 0)
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char *)(uintptr_t)(u64)address,
                                (int)length, wide, wide_count);
        wide[wide_count] = L'\0';
        GlobalUnlock(block);
        if (!OpenClipboard(NULL)) { GlobalFree(block); return -(i64)ERR_BUSY; }
        if (!EmptyClipboard() || SetClipboardData(CF_UNICODETEXT, block) == NULL) {
            CloseClipboard(); GlobalFree(block); return -(i64)ERR_IO;
        }
        CloseClipboard(); /* ownership of block transferred to the system */
        return length;
    }
    return -(i64)ERR_BAD_ADDRESS;
}

/* SYS_ENV_READ returns UTF-8 bytes, matching the language string ABI. The
 * caller first queries the length, then supplies a buffer; a short buffer is
 * left untouched. Windows itself stores process environment strings as UTF-16. */
static i64 windows_env_read(i64 name_address, i64 name_length,
                            i64 output_address, i64 output_capacity) {
    if (name_address == 0 || name_length <= 0 || name_length > 4096)
        return -(i64)ERR_INVALID_ARGUMENT;
    const char *name_bytes = (const char *)(uintptr_t)(u64)name_address;
    if (memchr(name_bytes, 0, (size_t)name_length) != NULL)
        return -(i64)ERR_INVALID_ARGUMENT;
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          name_bytes, (int)name_length, NULL, 0);
    if (wide_length <= 0 || wide_length > 4096)
        return -(i64)ERR_INVALID_ARGUMENT;
    WCHAR *name = (WCHAR *)malloc(((size_t)wide_length + 1u) * sizeof(WCHAR));
    if (name == NULL) return -(i64)ERR_NO_MEMORY;
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        name_bytes, (int)name_length, name, wide_length);
    if (converted != wide_length) {
        free(name);
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    name[wide_length] = L'\0';
    SetLastError(ERROR_SUCCESS);
    DWORD required = GetEnvironmentVariableW(name, NULL, 0);
    DWORD lookup_error = GetLastError();
    if (required == 0) {
        free(name);
        return lookup_error == ERROR_ENVVAR_NOT_FOUND ? -(i64)ERR_NOT_FOUND : -(i64)ERR_IO;
    }
    if (required > 32768) {
        free(name);
        return -(i64)ERR_NAME_TOO_LONG;
    }
    WCHAR *value = (WCHAR *)malloc(((size_t)required + 1u) * sizeof(WCHAR));
    if (value == NULL) {
        free(name);
        return -(i64)ERR_NO_MEMORY;
    }
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(name, value, required + 1u);
    DWORD value_error = GetLastError();
    free(name);
    if (length == 0 && value_error == ERROR_ENVVAR_NOT_FOUND) {
        free(value);
        return -(i64)ERR_NOT_FOUND;
    }
    if (length > required || (length == 0 && value_error != ERROR_SUCCESS)) {
        free(value);
        return -(i64)ERR_IO;
    }
    int bytes = length == 0 ? 0 : WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, (int)length, NULL, 0, NULL, NULL);
    if (bytes == 0 && length != 0) {
        free(value);
        return -(i64)ERR_IO;
    }
    i64 result = (i64)bytes;
    if (output_address != 0 && output_capacity >= result && bytes > 0) {
        int copied = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value, (int)length, (char *)(uintptr_t)(u64)output_address,
            bytes, NULL, NULL);
        if (copied != bytes) result = -(i64)ERR_IO;
    }
    free(value);
    return result;
}

/* ---- 공통 디스패치 ------------------------------------------------------------------- */

/* main() is generated without argc/argv. CommandLineToArgvW follows the
 * Windows quoting rules and preserves Unicode before conversion to the
 * runtime's NUL-separated UTF-8 argv block. The command line is immutable
 * during the process lifetime, so the size/read pair sees the same input. */
static i64 windows_arg_size_or_read(i64 out_address, i64 capacity, int read_args) {
    if (out_address == 0 || (read_args && capacity < 0))
        return -(i64)ERR_INVALID_ARGUMENT;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL || argc < 0) return -(i64)ERR_NO_MEMORY;
    u64 total = 0;
    for (int index = 0; index < argc; ++index) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            argv[index], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0 || total > (u64)INT64_MAX - (u64)bytes) {
            LocalFree(argv);
            return -(i64)ERR_INVALID_ARGUMENT;
        }
        total += (u64)bytes;
    }
    if (!read_args) {
        u64 *out = (u64 *)(uintptr_t)(u64)out_address;
        out[0] = (u64)argc;
        out[1] = total;
        LocalFree(argv);
        return 0;
    }
    if ((u64)capacity < total) {
        LocalFree(argv);
        return -(i64)ERR_INVALID_ARGUMENT;
    }
    char *cursor = (char *)(uintptr_t)(u64)out_address;
    for (int index = 0; index < argc; ++index) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            argv[index], -1, cursor, (int)((u64)capacity < (u64)INT_MAX ?
                (u64)capacity : (u64)INT_MAX), NULL, NULL);
        if (bytes <= 0) {
            LocalFree(argv);
            return -(i64)ERR_INVALID_ARGUMENT;
        }
        cursor += bytes;
        capacity -= bytes;
    }
    LocalFree(argv);
    return (i64)total;
}

static i64 hmje_windows_dispatch(i64 number, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    (void)a5;
    (void)a6;
    switch (number) {
    case SYS_EXIT:
        ExitProcess((UINT)a1);
    case SYS_WRITE: {
        HANDLE out = a1 == 2 ? GetStdHandle(STD_ERROR_HANDLE) : GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(out, (LPCVOID)(uintptr_t)(u64)a2, (DWORD)a3, &written, NULL);
        return (i64)written;
    }
    case SYS_READ: {
        HANDLE input = a1 == 0 ? GetStdHandle(STD_INPUT_HANDLE) : (HANDLE)(uintptr_t)a1;
        DWORD read_bytes = 0;
        BOOL ok = ReadFile(input, (LPVOID)(uintptr_t)(u64)a2, (DWORD)a3, &read_bytes, NULL);
        return ok ? (i64)read_bytes : HMJE_WINDOWS_ENOSYS;
    }
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
        Sleep((DWORD)a1);
        return 0;
    case SYS_SLEEP_US:
        Sleep((DWORD)((a1 + 999) / 1000));
        return 0;
    case SYS_ALLOC: {
        void *block = malloc((size_t)(u64)a1);
        return block == NULL ? 0 : (i64)(u64)(uintptr_t)block;
    }
    case SYS_FREE:
        free((void *)(uintptr_t)(u64)a1);
        return 0;
    case SYS_PROCESS_ID:
        return (i64)GetCurrentProcessId();
    case SYS_YIELD:
        SwitchToThread();
        return 0;
    case SYS_VM_RESERVE:
        return vm_reserve((u64)a1);
    case SYS_VM_RELEASE:
        return vm_release((u64)a1);
    case SYS_OBJECT_OPEN_GRAPH_WALK:
        return object_open_graph_walk(a1, a2);
    case SYS_CLOSE:
        return windows_close_handle_or_socket(a1);
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
    case SYS_SOCKET_CONNECT:
        return windows_socket_connect(a1, a2, a3, a4);
    case SYS_SOCKET_SEND:
        return a4 == 0 ? windows_socket_send(a1, a2, a3) : windows_socket_send_flags(a1, a2, a3, a4);
    case SYS_SOCKET_LISTEN:
        return windows_socket_listen(a1, a2, a3, a4, a5, a6);
    case SYS_SOCKET_ACCEPT:
        return windows_socket_accept(a1, a2);
    case SYS_SOCKET_RECV:
        return windows_socket_recv(a1, a2, a3, a4);
    case SYS_GPU_COMMAND:
        return hmje_windows_gpu_command(a1, a2, a3);
    case SYS_FILE_LOCK:
        return file_lock(a1, a2, a3);
    case SYS_FILE_UNLINK:
        return file_unlink(a1, a2, a3);
    case SYS_FILE_IDENTITY:
        return file_identity(a1, a2, a3);
    case SYS_FRAMEBUFFER_INFO:
        return hmje_windows_framebuffer_info(a1, a2);
    case SYS_FRAMEBUFFER_BLIT:
        return hmje_windows_framebuffer_blit(a1, a2, a3, a4, a5, a6);
    case SYS_FRAMEBUFFER_PRESENT:
        return hmje_windows_framebuffer_present(a1, a2);
    case SYS_INPUT_POLL:
        return hmje_windows_input_poll();
    case SYS_INPUT_POLL2:
        return hmje_windows_input_poll2(a1, a2);
    case SYS_ARG_SIZES:
        return windows_arg_size_or_read(a1, 0, 0);
    case SYS_ARG_READ:
        return windows_arg_size_or_read(a1, a2, 1);
    case SYS_ENV_READ:
        return windows_env_read(a1, a2, a3, a4);
    case SYS_UI_SYSTEM_THEME: {
        char override[16] = {0};
        DWORD length = GetEnvironmentVariableA("HMJE_SYSTEM_THEME", override, sizeof(override));
        if (length == 4 && strcmp(override, "dark") == 0) return 2;
        if (length == 5 && strcmp(override, "light") == 0) return 1;
        return hmje_windows_system_theme();
    }
    case SYS_TEXT_INPUT_CONTROL2:
        /* a1 = 2: where the UI drew the composition (IME window placement).
         * Secure native input (a1 = 1) is not implemented here. */
        if (a1 == 2) return hmje_windows_text_input_caret(a2, a3);
        return HMJE_WINDOWS_ENOSYS;
    case SYS_TEXT_INPUT_CONTROL:
        return hmje_windows_text_input_control(a1);
    case SYS_UI_WINDOW_CONFIG:
        return hmje_windows_ui_window_config(a1, a2, a3);
    case SYS_UI_CLIPBOARD:
        return windows_ui_clipboard(a1, a2, a3);
    case SYS_UI_WINDOW:
        return hmje_windows_ui_window(a1, a2, a3);
    case SYS_APP_OPEN_URL:
        return hmje_windows_app_open_url(a1, a2, a3);
    case SYS_UI_DRAG_DROP:
        return hmje_windows_ui_drag_drop(a1, a2, a3);
    default:
        return HMJE_WINDOWS_ENOSYS;
    }
}

/* ---- runtime/user/syscall.hmje가 요구하는 4개 심볼 (고정 계약) ----------------------- */

i64 hmje_arch_syscall0(i64 number) {
    return hmje_windows_dispatch(number, 0, 0, 0, 0, 0, 0);
}

i64 hmje_arch_syscall1(i64 number, i64 arg1) {
    return hmje_windows_dispatch(number, arg1, 0, 0, 0, 0, 0);
}

i64 hmje_arch_syscall3(i64 number, i64 arg1, i64 arg2, i64 arg3) {
    return hmje_windows_dispatch(number, arg1, arg2, arg3, 0, 0, 0);
}

i64 hmje_arch_syscall6(i64 number, i64 arg1, i64 arg2, i64 arg3, i64 arg4, i64 arg5, i64 arg6) {
    return hmje_windows_dispatch(number, arg1, arg2, arg3, arg4, arg5, arg6);
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
