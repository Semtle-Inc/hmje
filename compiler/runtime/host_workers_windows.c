#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>
#include <process.h>
#include <stdint.h>
#include <stddef.h>
#include "host_files.h"
#include "host_process.h"

/* Each descriptor is {argument-pairs address, argc} for process jobs or
 * {path address, length} for hash jobs. The caller pins all input spans and
 * result storage until every worker has joined. */
struct hmje_compile_pool {
    const uint64_t *jobs;
    int64_t *results;
    uint64_t count;
    volatile LONG64 next;
    int hash_files;
    uint64_t (*callback)(uint64_t);
};

static unsigned __stdcall hmje_compile_worker(void *opaque) {
    struct hmje_compile_pool *pool = opaque;
    for (;;) {
        uint64_t index = (uint64_t)InterlockedIncrement64(&pool->next) - 1;
        if (index >= pool->count) return 0;
        if (pool->callback) {
            pool->results[index] = (int64_t)pool->callback(pool->jobs[index * 2]);
        } else if (pool->hash_files) {
            ((uint64_t *)pool->results)[index] = hmje_host_file_content_hash(
                pool->jobs[index * 2], pool->jobs[index * 2 + 1]);
        } else {
            pool->results[index] = hmje_host_process_run(
                pool->jobs[index * 2], pool->jobs[index * 2 + 1]);
        }
    }
}

static int64_t hmje_compiler_tasks(uint64_t jobs, uint64_t count, uint64_t results,
                                    uint64_t workers, int hash_files,
                                    uint64_t callback) {
    if (!count) return 0;
    if (!jobs || !results || count > 4096 || !workers || workers > 64) return -EINVAL;
    if (workers > count) workers = count;
    struct hmje_compile_pool pool = {
        .jobs = (const uint64_t *)(uintptr_t)jobs,
        .results = (int64_t *)(uintptr_t)results,
        .count = count,
        .next = 0,
        .hash_files = hash_files,
        .callback = (uint64_t (*)(uint64_t))(uintptr_t)callback
    };
    HANDLE threads[63];
    uint64_t started = 0;
    /* Resource exhaustion reduces concurrency, never drops queued work. */
    for (; started + 1 < workers; ++started) {
        /* Semantic callbacks recurse through the full HMJE frontend; reserve
         * a large virtual stack without committing it up front. Process/hash
         * jobs retain the executable's default stack. */
        unsigned stack = pool.callback ? (64u << 20) : 0;
        unsigned flags = pool.callback ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0;
        uintptr_t thread = _beginthreadex(NULL, stack, hmje_compile_worker,
                                          &pool, flags, NULL);
        if (!thread) break;
        threads[started] = (HANDLE)thread;
    }
    hmje_compile_worker(&pool);
    if (started) {
        DWORD wait = WaitForMultipleObjects((DWORD)started, threads, TRUE, INFINITE);
        if (wait == WAIT_FAILED) {
            /* Still join each live thread before releasing caller-owned spans. */
            for (uint64_t i = 0; i < started; ++i)
                WaitForSingleObject(threads[i], INFINITE);
        }
    }
    for (uint64_t i = 0; i < started; ++i) CloseHandle(threads[i]);
    return 0;
}

int64_t hmje_compiler_run_jobs(uint64_t jobs, uint64_t count, uint64_t results,
                               uint64_t workers) {
    return hmje_compiler_tasks(jobs, count, results, workers, 0, 0);
}

int64_t hmje_compiler_hash_files(uint64_t paths, uint64_t count, uint64_t hashes,
                                 uint64_t workers) {
    return hmje_compiler_tasks(paths, count, hashes, workers, 1, 0);
}

/* HMJE's threaded ARC/allocator mode is a required caller precondition. */
int64_t hmje_compiler_run_callbacks(uint64_t jobs, uint64_t count, uint64_t results,
                                    uint64_t workers, uint64_t callback) {
    if (!callback) return -EINVAL;
    return hmje_compiler_tasks(jobs, count, results, workers, 0, callback);
}

uint64_t hmje_compiler_cpu_count(void) {
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return cpus < 1 ? 1 : cpus;
}

uint64_t hmje_compiler_available_memory(void) {
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? (uint64_t)status.ullAvailPhys : 0;
}

uint64_t hmje_compiler_default_jobs(void) {
    uint64_t cpus = hmje_compiler_cpu_count();
    return cpus > 4 ? 4 : cpus;
}
#endif
