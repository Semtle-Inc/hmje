/* The three pthread operations used by 기반/thread.hmje, with a pointer-sized
 * result preserved across the Windows DWORD thread entry ABI. */
#include <process.h>
#include <errno.h>

uint64_t hmje_platform_performance_cpu_count(void) {
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count != 0) return (uint64_t)count;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (uint64_t)info.dwNumberOfProcessors : 1;
}

typedef struct HmjeWindowsThread {
    HANDLE handle;
    void *(*start)(void *);
    void *argument;
    void *result;
    volatile LONG references;
    volatile LONG claimed;
} HmjeWindowsThread;

static void hmje_windows_thread_release(HmjeWindowsThread *thread) {
    if (InterlockedDecrement(&thread->references) == 0) free(thread);
}

static unsigned __stdcall hmje_windows_thread_start(void *argument) {
    HmjeWindowsThread *thread = argument;
    thread->result = thread->start(thread->argument);
    hmje_windows_thread_release(thread);
    return 0;
}

int pthread_create(uint64_t *identifier, const void *attributes,
    void *(*start)(void *), void *argument) {
    if (!identifier || !start || attributes) return EINVAL;
    *identifier = 0;
    HmjeWindowsThread *thread = calloc(1, sizeof(*thread));
    if (!thread) return ENOMEM;
    thread->start = start;
    thread->argument = argument;
    thread->references = 2;
    uintptr_t handle = _beginthreadex(NULL, 0, hmje_windows_thread_start,
        thread, 0, NULL);
    if (!handle) { free(thread); return EAGAIN; }
    thread->handle = (HANDLE)handle;
    *identifier = (uint64_t)(uintptr_t)thread;
    return 0;
}

int pthread_join(uint64_t identifier, void **result) {
    if (!identifier) return EINVAL;
    HmjeWindowsThread *thread = (HmjeWindowsThread *)(uintptr_t)identifier;
    if (GetThreadId(thread->handle) == GetCurrentThreadId()) return EDEADLK;
    if (InterlockedCompareExchange(&thread->claimed, 1, 0) != 0) return EINVAL;
    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) {
        InterlockedExchange(&thread->claimed, 0);
        return EINVAL;
    }
    if (result) *result = thread->result;
    CloseHandle(thread->handle);
    hmje_windows_thread_release(thread);
    return 0;
}

int pthread_detach(uint64_t identifier) {
    if (!identifier) return EINVAL;
    HmjeWindowsThread *thread = (HmjeWindowsThread *)(uintptr_t)identifier;
    if (InterlockedCompareExchange(&thread->claimed, 2, 0) != 0) return EINVAL;
    if (!CloseHandle(thread->handle)) {
        InterlockedExchange(&thread->claimed, 0);
        return EINVAL;
    }
    hmje_windows_thread_release(thread);
    return 0;
}
