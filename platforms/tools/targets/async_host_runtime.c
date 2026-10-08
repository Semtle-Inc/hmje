#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "async_host_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct HmjeAsyncEvent {
    uint64_t token;
    uint64_t value;
    int32_t status;
    int32_t ready;
    int32_t abandoned;
    void (*cleanup)(uint64_t);
    struct HmjeAsyncEvent *next;
} HmjeAsyncEvent;

typedef struct HmjeAsyncHandleState {
    void *handle;
    int32_t blocked;
    int32_t cancelled;
    uint32_t cancellation_shield_depth;
    uint64_t generation;
    int64_t failure;
    struct HmjeAsyncHandleState *next;
} HmjeAsyncHandleState;

typedef struct HmjeAsyncTimer {
    uint64_t deadline_milliseconds;
    uint64_t token;
    void *handle;
    uint64_t handle_generation;
    int32_t kind;
    struct HmjeAsyncTimer *next;
} HmjeAsyncTimer;

typedef struct HmjeAsyncLimitWaiter {
    uint64_t token;
    struct HmjeAsyncLimitWaiter *next;
} HmjeAsyncLimitWaiter;

typedef struct HmjeAsyncLimit {
    uint64_t identifier;
    uint64_t capacity;
    uint64_t in_use;
    HmjeAsyncLimitWaiter *wait_head;
    HmjeAsyncLimitWaiter *wait_tail;
    struct HmjeAsyncLimit *next;
} HmjeAsyncLimit;

typedef struct HmjeAsyncPermit {
    uint64_t identifier;
    HmjeAsyncLimit *limit;
    struct HmjeAsyncPermit *next;
} HmjeAsyncPermit;

typedef struct HmjeAsyncChannelWaiter {
    uint64_t channel;
    uint64_t operation;
    uint64_t token;
    struct HmjeAsyncChannelWaiter *next;
} HmjeAsyncChannelWaiter;

static pthread_mutex_t hmje_async_lock = PTHREAD_MUTEX_INITIALIZER;
/* POSIX condition clock setup belongs at the pthread/OS boundary. */
static int hmje_async_condition_init(pthread_cond_t *condition) {
#if defined(__APPLE__)
    return pthread_cond_init(condition, NULL);
#else
    pthread_condattr_t attributes;
    int status = pthread_condattr_init(&attributes);
    if (status != 0) return status;
    status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (status == 0) status = pthread_cond_init(condition, &attributes);
    pthread_condattr_destroy(&attributes);
    return status;
#endif
}

static struct timespec hmje_async_monotonic_after(uint64_t milliseconds) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) __builtin_trap();
    uint64_t seconds = milliseconds / UINT64_C(1000);
    /* Saturate before converting an untrusted u64 duration to signed time_t. */
    if (seconds > (uint64_t)(INT64_MAX - deadline.tv_sec - 1)) {
        deadline.tv_sec = INT64_MAX;
        deadline.tv_nsec = 999999999;
        return deadline;
    }
    deadline.tv_sec += (time_t)seconds;
    deadline.tv_nsec += (long)((milliseconds % 1000) * 1000000);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static int hmje_async_condition_wait_until(pthread_cond_t *condition,
    pthread_mutex_t *lock, const struct timespec *deadline) {
#if defined(__APPLE__)
    struct timespec now, remaining;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) __builtin_trap();
    if (now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
        return ETIMEDOUT;
    remaining.tv_sec = deadline->tv_sec - now.tv_sec;
    remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (remaining.tv_nsec < 0) {
        remaining.tv_sec -= 1;
        remaining.tv_nsec += 1000000000L;
    }
    return pthread_cond_timedwait_relative_np(condition, lock, &remaining);
#else
    return pthread_cond_timedwait(condition, lock, deadline);
#endif
}

static pthread_cond_t hmje_async_wake_storage;
static pthread_once_t hmje_async_wake_once = PTHREAD_ONCE_INIT;
static void hmje_async_wake_initialize(void) {
    if (hmje_async_condition_init(&hmje_async_wake_storage) != 0)
        __builtin_trap();
}
static pthread_cond_t *hmje_async_wake_address(void) {
    pthread_once(&hmje_async_wake_once, hmje_async_wake_initialize);
    return &hmje_async_wake_storage;
}
#define hmje_async_wake_condition (*hmje_async_wake_address())

static uint64_t hmje_async_next_token = 1;
static uint64_t hmje_async_pending_wakes = 0;
static uint64_t hmje_async_blocking_wait_count = 0;
/* Monotonic wake generation.  Every completion/cancel/failure bumps it; the
 * host poll loop records the value it last observed.  A host sleep (SYS_SLEEP_MS)
 * compares the two so a UI frame loop that ticks tasks once per frame can wake
 * as soon as a native completion lands instead of at the next frame. */
static uint64_t hmje_async_wake_generation = 0;
static uint64_t hmje_async_wake_generation_seen = 0;
/* Display refresh generation.  A platform that has a vsync source (iOS
 * CADisplayLink) bumps it once per refresh; hmje_async_sleep_until_wake_ms
 * reports it as bit 1 so the UI frame loop can start the next frame on the
 * refresh instead of on a free-running timer.  Platforms without a source
 * never bump it and the sleep behaves exactly as before. */
static uint64_t hmje_async_frame_generation = 0;
static uint64_t hmje_async_frame_generation_seen = 0;
static HmjeAsyncEvent *hmje_async_events = NULL;
static HmjeAsyncHandleState *hmje_async_handles = NULL;
/* Handle addresses can be reused; deadline workers retain this identity only. */
static uint64_t hmje_async_next_handle_generation = 1;
static uint64_t hmje_async_deadline_handle_generation(void *handle);
static void hmje_async_deadline_publish_failure(void *handle, uint64_t generation);
static HmjeAsyncLimit *hmje_async_limits = NULL;
static HmjeAsyncPermit *hmje_async_permits = NULL;
static uint64_t hmje_async_next_limit_identifier = 1;
static uint64_t hmje_async_next_permit_identifier = 1;
static pthread_mutex_t hmje_async_channel_waiter_lock = PTHREAD_MUTEX_INITIALIZER;
static HmjeAsyncChannelWaiter *hmje_async_channel_waiters = NULL;

/* GUI hosts may replace these hooks with a native event wait.  The default
 * keeps the pthread condition path used by headless POSIX and tests. */
__attribute__((weak)) int32_t hmje_async_host_wait_ms(uint64_t timeout_ms) {
    (void)timeout_ms;
    return -1;
}

__attribute__((weak)) void hmje_async_host_wake(void) {
}

static void hmje_async_channel_lock(uint64_t channel) {
    uint64_t *lock = (uint64_t *)(uintptr_t)channel;
    while (__atomic_exchange_n(lock, UINT64_C(1), __ATOMIC_ACQUIRE) != 0) {
        sched_yield();
    }
}

static void hmje_async_channel_unlock(uint64_t channel) {
    uint64_t *lock = (uint64_t *)(uintptr_t)channel;
    __atomic_store_n(lock, UINT64_C(0), __ATOMIC_RELEASE);
}

static int hmje_async_channel_ready_locked(uint64_t channel, uint64_t operation) {
    uint8_t *base = (uint8_t *)(uintptr_t)channel;
    uint64_t capacity = *(uint64_t *)(void *)(base + 8);
    uint64_t stride = *(uint64_t *)(void *)(base + 24);
    uint64_t head = *(uint64_t *)(void *)(base + 32);
    uint64_t occupied = *(uint64_t *)(void *)(base + 48);
    uint64_t senders = *(uint64_t *)(void *)(base + 56);
    uint64_t receivers = *(uint64_t *)(void *)(base + 64);
    if (operation == UINT64_C(1)) {
        if (senders == 0) return 1;
        if (occupied == 0 || head >= capacity) return 0;
        uint64_t state = *(uint64_t *)(void *)(base + 152 + head * stride);
        return state == UINT64_C(3);
    }
    if (operation == UINT64_C(2)) {
        return receivers == 0 || occupied < capacity;
    }
    return 1;
}

void hmje_async_channel_wait(uint64_t channel, uint64_t operation, uint64_t token) {
    if (channel == 0 || (operation != 1 && operation != 2)) {
        hmje_async_complete_void(token, EINVAL);
        return;
    }
    HmjeAsyncChannelWaiter *waiter =
        (HmjeAsyncChannelWaiter *)malloc(sizeof(HmjeAsyncChannelWaiter));
    if (waiter == NULL) {
        hmje_async_complete_void(token, ENOMEM);
        return;
    }
    waiter->channel = channel;
    waiter->operation = operation;
    waiter->token = token;

    hmje_async_channel_lock(channel);
    if (hmje_async_channel_ready_locked(channel, operation)) {
        hmje_async_channel_unlock(channel);
        free(waiter);
        hmje_async_complete_void(token, 0);
        return;
    }
    pthread_mutex_lock(&hmje_async_channel_waiter_lock);
    waiter->next = hmje_async_channel_waiters;
    hmje_async_channel_waiters = waiter;
    pthread_mutex_unlock(&hmje_async_channel_waiter_lock);
    hmje_async_channel_unlock(channel);
}

static void hmje_async_channel_wake(uint64_t channel, uint64_t operation, int destroy) {
    HmjeAsyncChannelWaiter *ready = NULL;
    pthread_mutex_lock(&hmje_async_channel_waiter_lock);
    HmjeAsyncChannelWaiter **link = &hmje_async_channel_waiters;
    while (*link != NULL) {
        HmjeAsyncChannelWaiter *waiter = *link;
        if (waiter->channel == channel &&
            (destroy || (waiter->operation & operation) != 0)) {
            *link = waiter->next;
            waiter->next = ready;
            ready = waiter;
            continue;
        }
        link = &waiter->next;
    }
    pthread_mutex_unlock(&hmje_async_channel_waiter_lock);
    while (ready != NULL) {
        HmjeAsyncChannelWaiter *waiter = ready;
        ready = waiter->next;
        hmje_async_complete_void(waiter->token, 0);
        free(waiter);
    }
}

void hmje_async_channel_notify(uint64_t channel, uint64_t operation) {
    if (channel != 0 && operation != 0) {
        hmje_async_channel_wake(channel, operation, 0);
    }
}

void hmje_async_channel_destroy(uint64_t channel) {
    if (channel != 0) {
        hmje_async_channel_wake(channel, UINT64_C(3), 1);
    }
}

typedef struct HmjeAsyncDbResult {
    uint8_t *bytes;
    uint64_t length;
} HmjeAsyncDbResult;

typedef struct HmjeAsyncDbJob {
    uint8_t *request;
    uint64_t request_length;
    uint64_t token;
    HmjeAsyncDbResult *result;
    int32_t status;
    int32_t done;
    int32_t cancelled;
    int32_t commit_started;
    void *owner;
    uint64_t opcode;
    uint64_t queued_at_ms;
    int32_t warned;
    char *source_location;
    pthread_cond_t done_condition;
    struct HmjeAsyncDbJob *next;
} HmjeAsyncDbJob;

typedef struct HmjeAsyncDbWorker {
    uint64_t identifier;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    HmjeAsyncDbJob *first;
    HmjeAsyncDbJob *last;
    HmjeAsyncDbJob *running;
    HmjeAsyncDbBytesCallback callback;
    uint64_t state;
    int32_t stopping;
    uint32_t sync_waiters;
    int32_t owner_cancelled;
    void *owner;
    char *owner_location;
    uint32_t transaction_depth;
    uint64_t begin_opcode;
    uint64_t commit_opcode;
    uint64_t rollback_opcode;
    uint64_t migration_opcode;
    struct HmjeAsyncDbWorker *registry_next;
} HmjeAsyncDbWorker;

typedef struct HmjeAsyncDbTaskScope {
    void *owner;
    struct HmjeAsyncDbTaskScope *previous;
} HmjeAsyncDbTaskScope;

typedef struct HmjeAsyncDbTaskChild {
    void *handle;
    void *owner;
    struct HmjeAsyncDbTaskChild *next;
} HmjeAsyncDbTaskChild;

static _Thread_local HmjeAsyncDbTaskScope *hmje_async_db_task_scope;
static _Thread_local HmjeAsyncDbWorker *hmje_async_db_current_worker;
static _Thread_local HmjeAsyncDbJob *hmje_async_db_current_job;
static pthread_mutex_t hmje_async_db_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static HmjeAsyncDbWorker *hmje_async_db_workers;
static uint64_t hmje_async_db_next_identifier = 1;
static HmjeAsyncDbTaskChild *hmje_async_db_task_children;

static void hmje_async_db_register_child(void *handle) {
    if (hmje_async_db_task_scope == NULL || handle == NULL ||
        handle == hmje_async_db_task_scope->owner) return;
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbTaskChild *child = hmje_async_db_task_children;
    while (child != NULL && child->handle != handle) child = child->next;
    if (child == NULL) {
        child = malloc(sizeof(*child));
        if (child == NULL) __builtin_trap();
        child->handle = handle;
        child->next = hmje_async_db_task_children;
        hmje_async_db_task_children = child;
    }
    child->owner = hmje_async_db_task_scope->owner;
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
}

static void hmje_async_db_forget_child(void *handle) {
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbTaskChild **link = &hmje_async_db_task_children;
    while (*link != NULL && (*link)->handle != handle)
        link = &(*link)->next;
    HmjeAsyncDbTaskChild *child = *link;
    if (child != NULL) *link = child->next;
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    free(child);
}

void hmje_async_db_task_push(void *handle) {
    HmjeAsyncDbTaskScope *scope = malloc(sizeof(*scope));
    if (scope == NULL) __builtin_trap();
    scope->owner = hmje_async_db_task_scope == NULL
        ? handle : hmje_async_db_task_scope->owner;
    scope->previous = hmje_async_db_task_scope;
    hmje_async_db_task_scope = scope;
}

void hmje_async_db_task_pop(void) {
    HmjeAsyncDbTaskScope *scope = hmje_async_db_task_scope;
    if (scope == NULL) abort();
    hmje_async_db_task_scope = scope->previous;
    free(scope);
}

static uint64_t hmje_async_db_read_u64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= (uint64_t)bytes[index] << (index * 8);
    return value;
}

static void hmje_async_db_write_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8));
}

static uint64_t hmje_async_db_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static char *hmje_async_db_request_location(const uint8_t *request,
                                             uint64_t length) {
    if (length < 40 || hmje_async_db_read_u64(request) != 1) return NULL;
    uint64_t isolation_length = hmje_async_db_read_u64(request + 16);
    if (isolation_length > length - 40) return NULL;
    uint64_t position = 24 + isolation_length + 8;
    uint64_t source_length = hmje_async_db_read_u64(request + position);
    if (source_length == 0 || source_length > length - position - 8 ||
        source_length > 4096) return NULL;
    char *source = malloc((size_t)source_length + 1);
    if (source == NULL) return NULL;
    memcpy(source, request + position + 8, (size_t)source_length);
    for (uint64_t index = 0; index < source_length; ++index)
        if (source[index] == '\0') source[index] = '?';
    source[source_length] = '\0';
    return source;
}

static void hmje_async_db_cancel_owner(void *handle, int32_t follow_child) {
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    if (follow_child) {
        for (HmjeAsyncDbTaskChild *child = hmje_async_db_task_children;
             child != NULL; child = child->next)
            if (child->handle == handle) {
                handle = child->owner;
                break;
            }
    }
    for (HmjeAsyncDbWorker *worker = hmje_async_db_workers;
         worker != NULL; worker = worker->registry_next) {
        pthread_mutex_lock(&worker->lock);
        if (worker->owner == handle) worker->owner_cancelled = 1;
        if (worker->running != NULL && worker->running->owner == handle &&
            (worker->running->opcode == worker->begin_opcode ||
             (worker->running->opcode == worker->migration_opcode &&
              !worker->running->commit_started)))
            worker->running->cancelled = 1;
        for (HmjeAsyncDbJob *job = worker->first;
             job != NULL; job = job->next)
            if (job->owner == handle &&
                (job->opcode == worker->begin_opcode ||
                 job->opcode == worker->migration_opcode))
                job->cancelled = 1;
        pthread_cond_signal(&worker->ready);
        pthread_mutex_unlock(&worker->lock);
    }
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
}

static HmjeAsyncDbWorker *hmje_async_db_find_worker_locked(uint64_t identifier) {
    for (HmjeAsyncDbWorker *worker = hmje_async_db_workers;
         worker != NULL; worker = worker->registry_next)
        if (worker->identifier == identifier) return worker;
    return NULL;
}

int32_t hmje_async_db_worker_is_live(uint64_t identifier) {
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(identifier);
    int32_t live = 0;
    if (worker != NULL) {
        pthread_mutex_lock(&worker->lock);
        live = !worker->stopping;
        pthread_mutex_unlock(&worker->lock);
    }
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    return live;
}

void hmje_async_db_worker_set_migration_opcode(uint64_t identifier,
                                                uint64_t opcode) {
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(identifier);
    if (worker != NULL) {
        pthread_mutex_lock(&worker->lock);
        worker->migration_opcode = opcode;
        pthread_mutex_unlock(&worker->lock);
    }
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
}

/* The lock makes cancellation and the start of COMMIT a single ordered
 * decision. The lock is released before backend I/O so cancelling never
 * blocks the calling executor behind a slow COMMIT. */
int32_t hmje_async_db_migration_commit_enter(void) {
    HmjeAsyncDbWorker *worker = hmje_async_db_current_worker;
    HmjeAsyncDbJob *job = hmje_async_db_current_job;
    if (worker == NULL || job == NULL ||
        job->opcode != worker->migration_opcode) return 1;
    pthread_mutex_lock(&worker->lock);
    if (job->cancelled || worker->stopping) {
        pthread_mutex_unlock(&worker->lock);
        return 0;
    }
    job->commit_started = 1;
    pthread_mutex_unlock(&worker->lock);
    return 1;
}

uint64_t hmje_async_db_bytes_resize(uint64_t bytes, uint64_t size) {
    if (size > SIZE_MAX) return 0;
    return (uint64_t)(uintptr_t)realloc((void *)(uintptr_t)bytes,
                                        (size_t)(size == 0 ? 1 : size));
}

void hmje_async_db_bytes_free(uint64_t bytes) {
    free((void *)(uintptr_t)bytes);
}

void hmje_async_db_result_free(uint64_t result) {
    HmjeAsyncDbResult *value = (HmjeAsyncDbResult *)(uintptr_t)result;
    if (value != NULL) {
        free(value->bytes);
        free(value);
    }
}

uint64_t hmje_async_db_result_data(uint64_t result) {
    HmjeAsyncDbResult *value = (HmjeAsyncDbResult *)(uintptr_t)result;
    return value == NULL ? 0 : (uint64_t)(uintptr_t)value->bytes;
}

uint64_t hmje_async_db_result_length(uint64_t result) {
    HmjeAsyncDbResult *value = (HmjeAsyncDbResult *)(uintptr_t)result;
    return value == NULL ? 0 : value->length;
}

static void *hmje_async_db_worker_run(void *argument) {
    HmjeAsyncDbWorker *worker = (HmjeAsyncDbWorker *)argument;
    for (;;) {
        pthread_mutex_lock(&worker->lock);
        HmjeAsyncDbJob *job = NULL;
        for (;;) {
            if (worker->owner != NULL &&
                (worker->owner_cancelled || worker->stopping)) break;
            HmjeAsyncDbJob **link = &worker->first;
            if (worker->owner != NULL) {
                while (*link != NULL && (*link)->owner != worker->owner)
                    link = &(*link)->next;
            }
            if (*link != NULL) {
                job = *link;
                *link = job->next;
                if (worker->last == job) {
                    worker->last = NULL;
                    for (HmjeAsyncDbJob *tail = worker->first;
                         tail != NULL; tail = tail->next)
                        worker->last = tail;
                }
                worker->running = job;
                break;
            }
            if (worker->first == NULL && worker->stopping) {
                pthread_mutex_unlock(&worker->lock);
                return NULL;
            }
#ifndef NDEBUG
            if (worker->owner != NULL && worker->first != NULL) {
                uint64_t now = hmje_async_db_milliseconds();
                for (HmjeAsyncDbJob *held = worker->first;
                     held != NULL; held = held->next) {
                    if (!held->warned && now - held->queued_at_ms >= 5000) {
                        fprintf(stderr, "HMJE DB warning: request held >5 s behind open transaction owned by task %p at %s\n", worker->owner, worker->owner_location == NULL ? "<unknown>" : worker->owner_location);
                        held->warned = 1;
                    }
                }
                struct timespec until = hmje_async_monotonic_after(1000);
                hmje_async_condition_wait_until(&worker->ready, &worker->lock, &until);
            } else
#endif
                pthread_cond_wait(&worker->ready, &worker->lock);
        }
        if (job == NULL) {
            /* The owner has been cancelled.  Roll back before releasing held
             * work.  No HMJE value crosses this thread boundary. */
            uint8_t rollback[16];
            hmje_async_db_write_u64(rollback, 1);
            hmje_async_db_write_u64(rollback + 8, worker->rollback_opcode);
            pthread_mutex_unlock(&worker->lock);
            uint64_t ignored_length = 0;
            uint64_t ignored = worker->callback(
                worker->state, (uint64_t)(uintptr_t)rollback, sizeof(rollback),
                (uint64_t)(uintptr_t)&ignored_length);
            int rollback_succeeded = ignored != 0 && ignored_length >= 16 &&
                hmje_async_db_read_u64((const uint8_t *)(uintptr_t)ignored) == 1 &&
                hmje_async_db_read_u64((const uint8_t *)(uintptr_t)ignored + 8) != 0;
            free((void *)(uintptr_t)ignored);
            if (!rollback_succeeded) {
                /* A failed rollback cannot safely release the gate onto an
                 * open transaction. Close this connection on its worker. */
                uint8_t close_request[16];
                hmje_async_db_write_u64(close_request, 1);
                hmje_async_db_write_u64(close_request + 8, 3);
                uint64_t close_length = 0;
                uint64_t close_result = worker->callback(
                    worker->state, (uint64_t)(uintptr_t)close_request,
                    sizeof(close_request), (uint64_t)(uintptr_t)&close_length);
                free((void *)(uintptr_t)close_result);
            }
            pthread_mutex_lock(&worker->lock);
            worker->owner = NULL;
            free(worker->owner_location);
            worker->owner_location = NULL;
            worker->transaction_depth = 0;
            worker->owner_cancelled = 0;
            pthread_cond_signal(&worker->ready);
            pthread_mutex_unlock(&worker->lock);
            continue;
        }
        int cancelled_before_run = job->cancelled;
        pthread_mutex_unlock(&worker->lock);

        uint64_t result_length = 0;
        uint64_t result_bytes = 0;
        if (!cancelled_before_run) {
            hmje_async_db_current_worker = worker;
            hmje_async_db_current_job = job;
            result_bytes = worker->callback(
                worker->state, (uint64_t)(uintptr_t)job->request,
                job->request_length, (uint64_t)(uintptr_t)&result_length);
            hmje_async_db_current_job = NULL;
            hmje_async_db_current_worker = NULL;
        }
        free(job->request);
        job->request = NULL;
        pthread_mutex_lock(&worker->lock);
        if (worker->begin_opcode != 0 && !cancelled_before_run &&
            result_bytes != 0 && result_length >= 16 &&
            hmje_async_db_read_u64((const uint8_t *)(uintptr_t)result_bytes) == 1 &&
            hmje_async_db_read_u64((const uint8_t *)(uintptr_t)result_bytes + 8) != 0) {
            if (job->opcode == worker->begin_opcode && job->owner != NULL) {
                worker->owner = job->owner;
                worker->transaction_depth += 1;
                if (worker->transaction_depth == 1) {
                    free(worker->owner_location);
                    worker->owner_location = job->source_location;
                    job->source_location = NULL;
                }
                if (job->cancelled) worker->owner_cancelled = 1;
            } else if (job->owner == worker->owner &&
                       (job->opcode == worker->commit_opcode ||
                        job->opcode == worker->rollback_opcode)) {
                if (worker->transaction_depth > 0)
                    worker->transaction_depth -= 1;
                if (worker->transaction_depth == 0) {
                    worker->owner = NULL;
                    free(worker->owner_location);
                    worker->owner_location = NULL;
                    worker->owner_cancelled = 0;
                }
            }
        }
        worker->running = NULL;
        pthread_cond_signal(&worker->ready);
        pthread_mutex_unlock(&worker->lock);
        free(job->source_location);
        HmjeAsyncDbResult *result = malloc(sizeof(*result));
        if (cancelled_before_run || result == NULL ||
            (result_bytes == 0 && result_length != 0)) {
            free((void *)(uintptr_t)result_bytes);
            free(result);
            job->status = cancelled_before_run ? ECANCELED : ENOMEM;
        } else {
            result->bytes = (uint8_t *)(uintptr_t)result_bytes;
            result->length = result_length;
            job->result = result;
        }
        if (job->token != 0) {
            hmje_async_complete_owned_u64(
                job->token, job->status,
                (uint64_t)(uintptr_t)job->result,
                hmje_async_db_result_free);
            free(job);
        } else {
            pthread_mutex_lock(&worker->lock);
            job->done = 1;
            pthread_cond_signal(&job->done_condition);
            pthread_mutex_unlock(&worker->lock);
        }
    }
}

uint64_t hmje_async_db_worker_create(uint64_t state,
                                     HmjeAsyncDbBytesCallback callback) {
    if (callback == NULL) return 0;
    HmjeAsyncDbWorker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) return 0;
    worker->state = state;
    worker->callback = callback;
    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        free(worker);
        return 0;
    }
    if (hmje_async_condition_init(&worker->ready) != 0) {
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return 0;
    }
    if (pthread_create(&worker->thread, NULL, hmje_async_db_worker_run,
                       worker) != 0) {
        pthread_cond_destroy(&worker->ready);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return 0;
    }
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    worker->identifier = hmje_async_db_next_identifier++;
    if (hmje_async_db_next_identifier == 0) abort();
    worker->registry_next = hmje_async_db_workers;
    hmje_async_db_workers = worker;
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    return worker->identifier;
}

uint64_t hmje_async_db_worker_create_transactional(
    uint64_t state, HmjeAsyncDbBytesCallback callback,
    uint64_t begin_opcode, uint64_t commit_opcode, uint64_t rollback_opcode) {
    if (begin_opcode == 0 || commit_opcode == 0 || rollback_opcode == 0 ||
        begin_opcode == commit_opcode || begin_opcode == rollback_opcode ||
        commit_opcode == rollback_opcode) return 0;
    uint64_t handle = hmje_async_db_worker_create(state, callback);
    if (handle != 0) {
        pthread_mutex_lock(&hmje_async_db_registry_lock);
        HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
        pthread_mutex_lock(&worker->lock);
        worker->begin_opcode = begin_opcode;
        worker->commit_opcode = commit_opcode;
        worker->rollback_opcode = rollback_opcode;
        pthread_mutex_unlock(&worker->lock);
        pthread_mutex_unlock(&hmje_async_db_registry_lock);
    }
    return handle;
}

static int32_t hmje_async_db_worker_enqueue(
    uint64_t handle, uint64_t request, uint64_t request_length,
    uint64_t token, HmjeAsyncDbJob **out_job,
    HmjeAsyncDbWorker **out_worker) {
    if (handle == 0 || (request == 0 && request_length != 0) ||
        request_length > SIZE_MAX) return EINVAL;
    HmjeAsyncDbJob *job = calloc(1, sizeof(*job));
    if (job == NULL) return ENOMEM;
    if (request_length != 0) {
        job->request = malloc((size_t)request_length);
        if (job->request == NULL) {
            free(job);
            return ENOMEM;
        }
        memcpy(job->request, (const void *)(uintptr_t)request,
               (size_t)request_length);
    }
    job->request_length = request_length;
    job->token = token;
    job->owner = hmje_async_db_task_scope == NULL
        ? NULL : hmje_async_db_task_scope->owner;
    job->queued_at_ms = hmje_async_db_milliseconds();
    if (token == 0 && pthread_cond_init(&job->done_condition, NULL) != 0) {
        free(job->request);
        free(job->source_location);
        free(job);
        return ENOMEM;
    }
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
    if (worker == NULL) {
        pthread_mutex_unlock(&hmje_async_db_registry_lock);
        if (token == 0) pthread_cond_destroy(&job->done_condition);
        free(job->request);
        free(job);
        return EBADF;
    }
    pthread_mutex_lock(&worker->lock);
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    if (worker->stopping) {
        pthread_mutex_unlock(&worker->lock);
        if (token == 0) pthread_cond_destroy(&job->done_condition);
        free(job->request);
        free(job->source_location);
        free(job);
        return ECANCELED;
    }
    if (worker->begin_opcode != 0 && request_length >= 16 &&
        hmje_async_db_read_u64(job->request) == 1)
        job->opcode = hmje_async_db_read_u64(job->request + 8);
    if (job->opcode == worker->begin_opcode)
        job->source_location = hmje_async_db_request_location(
            job->request, job->request_length);
    if (worker->last == NULL) worker->first = job;
    else worker->last->next = job;
    worker->last = job;
    if (token == 0) worker->sync_waiters += 1;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->lock);
    *out_job = job;
    if (out_worker != NULL) *out_worker = worker;
    return 0;
}

int32_t hmje_async_db_worker_submit(uint64_t worker, uint64_t request,
                                    uint64_t request_length, uint64_t token) {
    if (token == 0) return EINVAL;
    HmjeAsyncDbJob *job = NULL;
    return hmje_async_db_worker_enqueue(
        worker, request, request_length, token, &job, NULL);
}

void hmje_async_db_worker_execute(uint64_t worker, uint64_t request,
                                  uint64_t request_length, uint64_t token) {
    int32_t status = hmje_async_db_worker_submit(
        worker, request, request_length, token);
    if (status != 0) hmje_async_complete_u64(token, status, 0);
}

int32_t hmje_async_db_worker_submit_sync(uint64_t worker, uint64_t request,
                                         uint64_t request_length,
                                         uint64_t result_address) {
    if (result_address == 0) return EINVAL;
    *(uint64_t *)(uintptr_t)result_address = 0;
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *target = hmje_async_db_find_worker_locked(worker);
    if (target != NULL && pthread_equal(pthread_self(), target->thread)) {
        pthread_mutex_unlock(&hmje_async_db_registry_lock);
        return EDEADLK;
    }
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    HmjeAsyncDbJob *job = NULL;
    int32_t status = hmje_async_db_worker_enqueue(
        worker, request, request_length, 0, &job, &target);
    if (status != 0) return status;
    pthread_mutex_lock(&target->lock);
    while (!job->done) pthread_cond_wait(&job->done_condition, &target->lock);
    target->sync_waiters -= 1;
    pthread_cond_signal(&target->ready);
    pthread_mutex_unlock(&target->lock);
    *(uint64_t *)(uintptr_t)result_address =
        (uint64_t)(uintptr_t)job->result;
    status = job->status;
    pthread_cond_destroy(&job->done_condition);
    free(job);
    return status;
}

void hmje_async_db_worker_destroy(uint64_t handle) {
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
    if (worker == NULL || pthread_equal(pthread_self(), worker->thread)) {
        pthread_mutex_unlock(&hmje_async_db_registry_lock);
        return;
    }
    pthread_mutex_lock(&worker->lock);
    if (worker->stopping) {
        pthread_mutex_unlock(&worker->lock);
        pthread_mutex_unlock(&hmje_async_db_registry_lock);
        return;
    }
    worker->stopping = 1;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->lock);
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    pthread_join(worker->thread, NULL);
    pthread_mutex_lock(&worker->lock);
    while (worker->sync_waiters != 0)
        pthread_cond_wait(&worker->ready, &worker->lock);
    pthread_mutex_unlock(&worker->lock);
    pthread_mutex_lock(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker **link = &hmje_async_db_workers;
    while (*link != NULL && *link != worker) link = &(*link)->registry_next;
    if (*link == worker) *link = worker->registry_next;
    pthread_mutex_unlock(&hmje_async_db_registry_lock);
    /* Submitters acquire worker->lock under the registry lock, then release
     * the registry. After unlinking, drain the last such critical section. */
    pthread_mutex_lock(&worker->lock);
    pthread_mutex_unlock(&worker->lock);
    pthread_cond_destroy(&worker->ready);
    pthread_mutex_destroy(&worker->lock);
    free(worker->owner_location);
    free(worker);
}

typedef struct HmjeAsyncDbClose {
    uint64_t worker;
    uint64_t token;
} HmjeAsyncDbClose;

static void *hmje_async_db_worker_reap(void *argument) {
    HmjeAsyncDbClose *close_request = (HmjeAsyncDbClose *)argument;
    hmje_async_db_worker_destroy(close_request->worker);
    hmje_async_complete_void(close_request->token, 0);
    free(close_request);
    return NULL;
}

void hmje_async_db_worker_destroy_async(uint64_t worker, uint64_t token) {
    HmjeAsyncDbClose *close_request = malloc(sizeof(*close_request));
    if (close_request == NULL) {
        hmje_async_complete_void(token, ENOMEM);
        return;
    }
    close_request->worker = worker;
    close_request->token = token;
    pthread_attr_t attributes;
    int init_status = pthread_attr_init(&attributes);
    int status = init_status;
    if (status == 0) status = pthread_attr_setdetachstate(
        &attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (status == 0) status = pthread_create(
        &thread, &attributes, hmje_async_db_worker_reap, close_request);
    if (init_status == 0) pthread_attr_destroy(&attributes);
    if (status != 0) {
        free(close_request);
        hmje_async_complete_void(token, status);
        return;
    }
}

/* Timer scheduling has its own lock so the worker never holds the completion
 * queue lock while publishing.  One detached worker serves all timers; an app
 * does not pay one pthread per sleep and an earlier insertion can wake and
 * reorder the scheduler.
 */
static pthread_mutex_t hmje_async_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hmje_async_timer_condition;
static pthread_once_t hmje_async_timer_once = PTHREAD_ONCE_INIT;
static HmjeAsyncTimer *hmje_async_timers = NULL;
static int32_t hmje_async_timer_start_status = 0;
static uint64_t hmje_async_next_deadline_token = 1;

static uint64_t hmje_async_monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        abort();
    }
    return ((uint64_t)now.tv_sec * UINT64_C(1000)) +
           ((uint64_t)now.tv_nsec / UINT64_C(1000000));
}

static void *hmje_async_timer_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&hmje_async_timer_lock);
        while (hmje_async_timers == NULL) {
            pthread_cond_wait(
                &hmje_async_timer_condition, &hmje_async_timer_lock);
        }

        uint64_t now = hmje_async_monotonic_milliseconds();
        HmjeAsyncTimer *timer = hmje_async_timers;
        if (timer->deadline_milliseconds > now) {
            struct timespec deadline = hmje_async_monotonic_after(
                timer->deadline_milliseconds - now);
            hmje_async_condition_wait_until(
                &hmje_async_timer_condition,
                &hmje_async_timer_lock,
                &deadline);
            pthread_mutex_unlock(&hmje_async_timer_lock);
            continue;
        }

        hmje_async_timers = timer->next;
        if (timer->kind == 1) {
            /* Remove the timer while holding the scheduler lock, but never
             * resume/cancel a coroutine under that lock.  Cancellation can
             * synchronously unwind a deadline scope, whose cleanup calls
             * hmje_async_deadline_cancel_for_handle and acquires this same
             * lock.  Keeping the lock here therefore deadlocks exactly when
             * a deadline expires.
             *
             * The timer is already detached from the list, so deadline_end
             * cannot free it while failure publication happens unlocked.
             */
            void *handle = timer->handle;
            pthread_mutex_unlock(&hmje_async_timer_lock);
            /* HMJE failure codes are platform-neutral; deadline is always -110. */
            hmje_async_deadline_publish_failure(handle, timer->handle_generation);
        } else {
            pthread_mutex_unlock(&hmje_async_timer_lock);
            hmje_async_complete_void(timer->token, 0);
        }
        free(timer);
    }
    return NULL;
}

static void hmje_async_timer_start_worker(void) {
    hmje_async_timer_start_status = hmje_async_condition_init(&hmje_async_timer_condition);
    if (hmje_async_timer_start_status != 0) return;
    pthread_t worker;
    int error = pthread_create(&worker, NULL, hmje_async_timer_worker, NULL);
    if (error != 0) {
        hmje_async_timer_start_status = error;
        return;
    }
    pthread_detach(worker);
}

void hmje_async_timer_wait(uint64_t delay_milliseconds, uint64_t token) {
    HmjeAsyncTimer *timer = (HmjeAsyncTimer *)malloc(sizeof(HmjeAsyncTimer));
    if (timer == NULL) {
        hmje_async_complete_void(token, ENOMEM);
        return;
    }
    pthread_once(&hmje_async_timer_once, hmje_async_timer_start_worker);
    if (hmje_async_timer_start_status != 0) {
        int32_t status = hmje_async_timer_start_status;
        free(timer);
        hmje_async_complete_void(token, status);
        return;
    }

    uint64_t now = hmje_async_monotonic_milliseconds();
    timer->deadline_milliseconds =
        delay_milliseconds > UINT64_MAX - now
            ? UINT64_MAX
            : now + delay_milliseconds;
    timer->token = token;
    timer->handle = NULL;
    timer->kind = 0;
    timer->next = NULL;

    pthread_mutex_lock(&hmje_async_timer_lock);
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL &&
           (*link)->deadline_milliseconds <= timer->deadline_milliseconds) {
        link = &(*link)->next;
    }
    int wake_worker = link == &hmje_async_timers;
    timer->next = *link;
    *link = timer;
    if (wake_worker) {
        pthread_cond_signal(&hmje_async_timer_condition);
    }
    pthread_mutex_unlock(&hmje_async_timer_lock);
}

uint64_t hmje_async_deadline_begin(uint64_t delay_milliseconds, void *handle) {
    if (handle == NULL) {
        abort();
    }
    HmjeAsyncTimer *timer = (HmjeAsyncTimer *)malloc(sizeof(HmjeAsyncTimer));
    if (timer == NULL) {
        __builtin_trap();
    }
    pthread_once(&hmje_async_timer_once, hmje_async_timer_start_worker);
    if (hmje_async_timer_start_status != 0) {
        free(timer);
        abort();
    }
    uint64_t now = hmje_async_monotonic_milliseconds();
    timer->deadline_milliseconds = delay_milliseconds > UINT64_MAX - now
        ? UINT64_MAX
        : now + delay_milliseconds;
    timer->handle = handle;
    timer->handle_generation = hmje_async_deadline_handle_generation(handle);
    timer->kind = 1;
    timer->next = NULL;
    pthread_mutex_lock(&hmje_async_timer_lock);
    timer->token = hmje_async_next_deadline_token++;
    if (timer->token == 0) {
        timer->token = hmje_async_next_deadline_token++;
    }
    uint64_t deadline_token = timer->token;
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL && (*link)->deadline_milliseconds <= timer->deadline_milliseconds) {
        link = &(*link)->next;
    }
    int wake_worker = link == &hmje_async_timers;
    timer->next = *link;
    *link = timer;
    if (wake_worker) {
        pthread_cond_signal(&hmje_async_timer_condition);
    }
    pthread_mutex_unlock(&hmje_async_timer_lock);
    return deadline_token;
}

void hmje_async_deadline_end(uint64_t deadline_token) {
    pthread_mutex_lock(&hmje_async_timer_lock);
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL) {
        HmjeAsyncTimer *timer = *link;
        if (timer->kind == 1 && timer->token == deadline_token) {
            *link = timer->next;
            free(timer);
            pthread_cond_signal(&hmje_async_timer_condition);
            break;
        }
        link = &timer->next;
    }
    pthread_mutex_unlock(&hmje_async_timer_lock);
}

void hmje_async_deadline_cancel_for_handle(void *handle) {
    pthread_mutex_lock(&hmje_async_timer_lock);
    int removed = 0;
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL) {
        HmjeAsyncTimer *timer = *link;
        if (timer->kind == 1 && timer->handle == handle) {
            *link = timer->next;
            free(timer);
            removed = 1;
            continue;
        }
        link = &timer->next;
    }
    /* A frame may be destroyed before any timer initializes this condition.
     * Only a registered timer can require a scheduler wake. */
    if (removed) pthread_cond_signal(&hmje_async_timer_condition);
    pthread_mutex_unlock(&hmje_async_timer_lock);
}

static HmjeAsyncEvent *hmje_async_find_event_locked(uint64_t token) {
    HmjeAsyncEvent *event = hmje_async_events;
    while (event != NULL && event->token != token) {
        event = event->next;
    }
    return event;
}

static HmjeAsyncHandleState *hmje_async_find_handle_locked(void *handle) {
    HmjeAsyncHandleState *state = hmje_async_handles;
    while (state != NULL && state->handle != handle) {
        state = state->next;
    }
    return state;
}

/* Register the live frame before scheduling. A later frame at the same
 * address receives a new identity even if an expired timer is already detached. */
static uint64_t hmje_async_deadline_handle_generation(void *handle) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = calloc(1, sizeof(*state));
        if (state == NULL) __builtin_trap();
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->generation == 0) {
        state->generation = hmje_async_next_handle_generation++;
        if (hmje_async_next_handle_generation == 0) abort();
    }
    uint64_t generation = state->generation;
    pthread_mutex_unlock(&hmje_async_lock);
    return generation;
}

static void hmje_async_deadline_publish_failure(void *handle, uint64_t generation) {
    int wake = 0;
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    /* Never recreate forgotten state or tag a replacement frame. */
    if (state != NULL && state->generation == generation && state->failure == 0) {
        state->failure = -110;
        hmje_async_pending_wakes += 1;
        hmje_async_wake_generation += 1;
        pthread_cond_signal(&hmje_async_wake_condition);
        wake = 1;
    }
    pthread_mutex_unlock(&hmje_async_lock);
    if (wake) hmje_async_host_wake();
}

static HmjeAsyncLimit *hmje_async_find_limit_locked(uint64_t identifier) {
    HmjeAsyncLimit *limit = hmje_async_limits;
    while (limit != NULL && limit->identifier != identifier) {
        limit = limit->next;
    }
    return limit;
}

static uint64_t hmje_async_next_nonzero_identifier(uint64_t *next) {
    uint64_t identifier = (*next)++;
    if (identifier == 0) {
        identifier = (*next)++;
    }
    return identifier;
}

static HmjeAsyncPermit *hmje_async_permit_create_locked(HmjeAsyncLimit *limit) {
    HmjeAsyncPermit *permit = (HmjeAsyncPermit *)malloc(sizeof(HmjeAsyncPermit));
    if (permit == NULL) {
        return NULL;
    }
    permit->identifier = hmje_async_next_nonzero_identifier(
        &hmje_async_next_permit_identifier);
    permit->limit = limit;
    permit->next = hmje_async_permits;
    hmje_async_permits = permit;
    limit->in_use += 1;
    return permit;
}

static void hmje_async_permit_cleanup(uint64_t permit) {
    hmje_async_permit_release(permit);
}

uint64_t hmje_async_limit_create(uint64_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    HmjeAsyncLimit *limit = (HmjeAsyncLimit *)calloc(1, sizeof(HmjeAsyncLimit));
    if (limit == NULL) {
        return 0;
    }
    pthread_mutex_lock(&hmje_async_lock);
    limit->identifier = hmje_async_next_nonzero_identifier(
        &hmje_async_next_limit_identifier);
    limit->capacity = capacity;
    limit->next = hmje_async_limits;
    hmje_async_limits = limit;
    pthread_mutex_unlock(&hmje_async_lock);
    return limit->identifier;
}

void hmje_async_limit_acquire(uint64_t identifier, uint64_t token) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncLimit *limit = hmje_async_find_limit_locked(identifier);
    if (limit == NULL) {
        pthread_mutex_unlock(&hmje_async_lock);
        hmje_async_complete_void(token, EINVAL);
        return;
    }
    if (limit->in_use < limit->capacity) {
        HmjeAsyncPermit *permit = hmje_async_permit_create_locked(limit);
        pthread_mutex_unlock(&hmje_async_lock);
        if (permit == NULL) {
            hmje_async_complete_void(token, ENOMEM);
            return;
        }
        hmje_async_complete_owned_u64(
            token, 0, permit->identifier, hmje_async_permit_cleanup);
        return;
    }
    HmjeAsyncLimitWaiter *waiter =
        (HmjeAsyncLimitWaiter *)malloc(sizeof(HmjeAsyncLimitWaiter));
    if (waiter == NULL) {
        pthread_mutex_unlock(&hmje_async_lock);
        hmje_async_complete_void(token, ENOMEM);
        return;
    }
    waiter->token = token;
    waiter->next = NULL;
    if (limit->wait_tail == NULL) {
        limit->wait_head = waiter;
    } else {
        limit->wait_tail->next = waiter;
    }
    limit->wait_tail = waiter;
    pthread_mutex_unlock(&hmje_async_lock);
}

void hmje_async_permit_release(uint64_t identifier) {
    uint64_t grant_token = 0;
    uint64_t grant_identifier = 0;
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncPermit **permit_link = &hmje_async_permits;
    while (*permit_link != NULL && (*permit_link)->identifier != identifier) {
        permit_link = &(*permit_link)->next;
    }
    HmjeAsyncPermit *permit = *permit_link;
    if (permit == NULL || permit->limit == NULL || permit->limit->in_use == 0) {
        pthread_mutex_unlock(&hmje_async_lock);
        abort();
    }
    HmjeAsyncLimit *limit = permit->limit;
    HmjeAsyncLimitWaiter *waiter = limit->wait_head;
    if (waiter != NULL) {
        limit->wait_head = waiter->next;
        if (limit->wait_head == NULL) {
            limit->wait_tail = NULL;
        }
        permit->identifier = hmje_async_next_nonzero_identifier(
            &hmje_async_next_permit_identifier);
        grant_token = waiter->token;
        grant_identifier = permit->identifier;
        free(waiter);
    } else {
        *permit_link = permit->next;
        limit->in_use -= 1;
        free(permit);
    }
    pthread_mutex_unlock(&hmje_async_lock);
    if (grant_token != 0) {
        hmje_async_complete_owned_u64(
            grant_token,
            0,
            grant_identifier,
            hmje_async_permit_cleanup);
    }
}

int64_t hmje_async_limit_destroy(uint64_t identifier) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncLimit **link = &hmje_async_limits;
    while (*link != NULL && (*link)->identifier != identifier) {
        link = &(*link)->next;
    }
    HmjeAsyncLimit *limit = *link;
    if (limit == NULL) {
        pthread_mutex_unlock(&hmje_async_lock);
        return -EINVAL;
    }
    if (limit->in_use != 0 || limit->wait_head != NULL) {
        pthread_mutex_unlock(&hmje_async_lock);
        return -EBUSY;
    }
    *link = limit->next;
    pthread_mutex_unlock(&hmje_async_lock);
    free(limit);
    return 0;
}

uint64_t hmje_async_event_create(void) {
    HmjeAsyncEvent *event = (HmjeAsyncEvent *)calloc(1, sizeof(HmjeAsyncEvent));
    if (event == NULL) {
        __builtin_trap();
    }
    pthread_mutex_lock(&hmje_async_lock);
    event->token = hmje_async_next_token++;
    if (event->token == 0) {
        event->token = hmje_async_next_token++;
    }
    event->next = hmje_async_events;
    hmje_async_events = event;
    pthread_mutex_unlock(&hmje_async_lock);
    return event->token;
}

void hmje_async_complete_owned_u64(
    uint64_t token,
    int32_t status,
    uint64_t value,
    void (*cleanup)(uint64_t)) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL || event->ready) {
        pthread_mutex_unlock(&hmje_async_lock);
        abort();
    }
    if (event->abandoned) {
        *link = event->next;
        pthread_mutex_unlock(&hmje_async_lock);
        if (cleanup != NULL) {
            cleanup(value);
        }
        free(event);
        return;
    }
    event->value = value;
    event->status = status;
    event->cleanup = cleanup;
    event->ready = 1;
    hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
    pthread_cond_signal(&hmje_async_wake_condition);
    pthread_mutex_unlock(&hmje_async_lock);
    hmje_async_host_wake();
}

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value) {
    hmje_async_complete_owned_u64(token, status, value, NULL);
}

void hmje_async_complete_void(uint64_t token, int32_t status) {
    hmje_async_complete_u64(token, status, 0);
}

int32_t hmje_async_event_is_ready(uint64_t token) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncEvent *event = hmje_async_find_event_locked(token);
    int32_t ready = event != NULL && event->ready;
    pthread_mutex_unlock(&hmje_async_lock);
    return ready;
}

int32_t hmje_async_event_take_u64(uint64_t token, uint64_t *value, int32_t *status) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL || !event->ready) {
        pthread_mutex_unlock(&hmje_async_lock);
        return 0;
    }
    *link = event->next;
    if (value != NULL) {
        *value = event->value;
    }
    if (status != NULL) {
        *status = event->status;
    }
    if (hmje_async_pending_wakes > 0) {
        hmje_async_pending_wakes -= 1;
    }
    pthread_mutex_unlock(&hmje_async_lock);
    free(event);
    return 1;
}

int32_t hmje_async_event_abandon(uint64_t token) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL) {
        pthread_mutex_unlock(&hmje_async_lock);
        return 0;
    }
    if (event->ready) {
        *link = event->next;
        if (hmje_async_pending_wakes > 0) {
            hmje_async_pending_wakes -= 1;
        }
        pthread_mutex_unlock(&hmje_async_lock);
        if (event->cleanup != NULL) {
            event->cleanup(event->value);
        }
        free(event);
        return 1;
    }
    event->abandoned = 1;
    pthread_mutex_unlock(&hmje_async_lock);
    return 1;
}

int32_t hmje_async_event_is_abandoned(uint64_t token) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncEvent *event = hmje_async_find_event_locked(token);
    int32_t abandoned = event != NULL && event->abandoned;
    pthread_mutex_unlock(&hmje_async_lock);
    return abandoned;
}

void hmje_async_handle_set_blocked(void *handle, int32_t blocked) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            pthread_mutex_unlock(&hmje_async_lock);
            __builtin_trap();
        }
        state->handle = handle;
        state->cancelled = 0;
        state->cancellation_shield_depth = 0;
        state->failure = 0;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    state->blocked = blocked != 0;
    pthread_mutex_unlock(&hmje_async_lock);
    if (blocked) hmje_async_db_register_child(handle);
}

int32_t hmje_async_handle_is_blocked(void *handle) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int32_t blocked = state != NULL && state->blocked;
    pthread_mutex_unlock(&hmje_async_lock);
    return blocked;
}

void hmje_async_handle_request_cancel(void *handle) {
    if (handle == NULL) {
        return;
    }
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            pthread_mutex_unlock(&hmje_async_lock);
            __builtin_trap();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (!state->cancelled) {
        state->cancelled = 1;
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        pthread_cond_signal(&hmje_async_wake_condition);
    }
    pthread_mutex_unlock(&hmje_async_lock);
    hmje_async_db_cancel_owner(handle, 1);
    hmje_async_host_wake();
}

void hmje_async_handle_request_cancel_address(uint64_t handle_address) {
    hmje_async_handle_request_cancel((void *)(uintptr_t)handle_address);
}

int32_t hmje_async_handle_is_cancelled(void *handle) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int32_t cancelled = state != NULL && state->cancelled &&
        state->cancellation_shield_depth == 0;
    pthread_mutex_unlock(&hmje_async_lock);
    return cancelled;
}

void hmje_async_cancellation_shield_enter(void *handle) {
    if (handle == NULL) abort();
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            pthread_mutex_unlock(&hmje_async_lock);
            __builtin_trap();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->cancellation_shield_depth == UINT32_MAX) {
        pthread_mutex_unlock(&hmje_async_lock);
        abort();
    }
    state->cancellation_shield_depth += 1;
    pthread_mutex_unlock(&hmje_async_lock);
}

void hmje_async_cancellation_shield_exit(void *handle) {
    if (handle == NULL) abort();
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL || state->cancellation_shield_depth == 0) {
        pthread_mutex_unlock(&hmje_async_lock);
        abort();
    }
    state->cancellation_shield_depth -= 1;
    if (state->cancellation_shield_depth == 0 && state->cancelled) {
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        pthread_cond_signal(&hmje_async_wake_condition);
    }
    pthread_mutex_unlock(&hmje_async_lock);
    hmje_async_host_wake();
}

void hmje_async_handle_set_failure(void *handle, int64_t status) {
    if (handle == NULL || status == 0) {
        return;
    }
    if (status > 0) {
        status = -status;
    }
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            pthread_mutex_unlock(&hmje_async_lock);
            __builtin_trap();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->failure == 0) {
        state->failure = status;
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        pthread_cond_signal(&hmje_async_wake_condition);
    }
    pthread_mutex_unlock(&hmje_async_lock);
    hmje_async_host_wake();
}

int64_t hmje_async_handle_failure(void *handle) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int64_t failure = state != NULL ? state->failure : 0;
    pthread_mutex_unlock(&hmje_async_lock);
    return failure;
}

int64_t hmje_async_handle_take_failure(void *handle) {
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int64_t failure = state != NULL ? state->failure : 0;
    if (state != NULL) {
        state->failure = 0;
    }
    pthread_mutex_unlock(&hmje_async_lock);
    return failure;
}

void hmje_async_handle_forget(void *handle) {
    hmje_async_db_cancel_owner(handle, 0);
    hmje_async_db_forget_child(handle);
    pthread_mutex_lock(&hmje_async_lock);
    HmjeAsyncHandleState **link = &hmje_async_handles;
    while (*link != NULL && (*link)->handle != handle) {
        link = &(*link)->next;
    }
    HmjeAsyncHandleState *state = *link;
    if (state != NULL) {
        *link = state->next;
    }
    pthread_mutex_unlock(&hmje_async_lock);
    free(state);
}

typedef void (*HmjeCoroutineEntry)(void *frame);

typedef struct HmjeCoroutineFrameHeader {
    HmjeCoroutineEntry resume;
    HmjeCoroutineEntry destroy;
} HmjeCoroutineFrameHeader;

int32_t hmje_async_handle_poll_once(void *handle) {
    if (handle == NULL) {
        abort();
    }
    HmjeCoroutineFrameHeader *frame = (HmjeCoroutineFrameHeader *)handle;
    if (frame->resume == NULL) {
        return 1;
    }
    /* pthread TLS must follow nested polls, but this scope never escapes the
     * synchronous resume call. Keep it on the native stack, without malloc. */
    HmjeAsyncDbTaskScope scope = {handle, hmje_async_db_task_scope};
    hmje_async_db_task_scope = &scope;
    frame->resume(handle);
    hmje_async_db_task_scope = scope.previous;
    return frame->resume == NULL;
}

void hmje_async_handle_destroy(void *handle) {
    if (handle == NULL) {
        return;
    }
    HmjeCoroutineFrameHeader *frame = (HmjeCoroutineFrameHeader *)handle;
    HmjeCoroutineEntry destroy = frame->destroy;
    hmje_async_handle_forget(handle);
    if (destroy != NULL) {
        destroy(handle);
    }
}

typedef void (*HmjeAsyncClosureRelease)(uint64_t context);

typedef struct HmjeAsyncUnstructuredNode {
    void *handle;
    uint64_t closure_context;
    HmjeAsyncClosureRelease closure_release;
    struct HmjeAsyncUnstructuredNode *next;
} HmjeAsyncUnstructuredNode;

static HmjeAsyncUnstructuredNode *hmje_async_unstructured_head = NULL;
static HmjeAsyncUnstructuredNode *hmje_async_unstructured_tail = NULL;
static int32_t hmje_async_unstructured_atexit_registered = 0;

static void hmje_async_unstructured_release_node(HmjeAsyncUnstructuredNode *node) {
    hmje_async_handle_destroy(node->handle);
    if (node->closure_release != NULL && node->closure_context != 0) {
        node->closure_release(node->closure_context);
    }
    free(node);
}

static void hmje_async_unstructured_shutdown(void) {
    HmjeAsyncUnstructuredNode *node = hmje_async_unstructured_head;
    hmje_async_unstructured_head = NULL;
    hmje_async_unstructured_tail = NULL;
    while (node != NULL) {
        HmjeAsyncUnstructuredNode *next = node->next;
        hmje_async_handle_request_cancel(node->handle);
        hmje_async_unstructured_release_node(node);
        node = next;
    }
}

void hmje_async_unstructured_add(
    void *handle, uint64_t closure_context, uint64_t closure_release_address) {
    if (handle == NULL) {
        abort();
    }
    HmjeAsyncUnstructuredNode *node =
        (HmjeAsyncUnstructuredNode *)calloc(1, sizeof(*node));
    if (node == NULL) {
        __builtin_trap();
    }
    node->handle = handle;
    node->closure_context = closure_context;
    node->closure_release =
        (HmjeAsyncClosureRelease)(uintptr_t)closure_release_address;
    /* `비동기 작업` has Task-like eager-start semantics.  Merely enqueueing
     * the initial-suspend handle means a root coroutine can block on an
     * unrelated long wait before the new task registers its first event; if
     * the root then finishes, the task never starts at all.  Run exactly to
     * the first suspension before publishing it to the executor queue. */
    if (hmje_async_handle_poll_once(handle)) {
        hmje_async_unstructured_release_node(node);
        return;
    }
    if (!hmje_async_unstructured_atexit_registered) {
        if (atexit(hmje_async_unstructured_shutdown) != 0) {
            abort();
        }
        hmje_async_unstructured_atexit_registered = 1;
    }
    if (hmje_async_unstructured_tail == NULL) {
        hmje_async_unstructured_head = node;
    } else {
        hmje_async_unstructured_tail->next = node;
    }
    hmje_async_unstructured_tail = node;
}

int32_t hmje_async_unstructured_poll(void) {
    int32_t all_blocked = 1;
    pthread_mutex_lock(&hmje_async_lock);
    hmje_async_wake_generation_seen = hmje_async_wake_generation;
    pthread_mutex_unlock(&hmje_async_lock);
    HmjeAsyncUnstructuredNode **link = &hmje_async_unstructured_head;
    while (*link != NULL) {
        HmjeAsyncUnstructuredNode *node = *link;
        if (!hmje_async_handle_poll_once(node->handle)) {
            if (!hmje_async_handle_is_blocked(node->handle)) {
                all_blocked = 0;
            }
            link = &node->next;
            continue;
        }
        *link = node->next;
        hmje_async_unstructured_release_node(node);
    }
    hmje_async_unstructured_tail = NULL;
    for (HmjeAsyncUnstructuredNode *node = hmje_async_unstructured_head;
         node != NULL; node = node->next) {
        hmje_async_unstructured_tail = node;
    }
    if (hmje_async_unstructured_head == NULL) {
        return 2;
    }
    return all_blocked ? 1 : 0;
}

void hmje_async_unstructured_tick(void) {
    (void)hmje_async_unstructured_poll();
}

typedef struct HmjeAsyncGroupNode {
    void *handle;
    struct HmjeAsyncGroupNode *next;
} HmjeAsyncGroupNode;

typedef struct HmjeAsyncGroup {
    HmjeAsyncGroupNode *head;
    HmjeAsyncGroupNode *tail;
    int64_t first_failure;
    int32_t any_cancelled;
} HmjeAsyncGroup;

void *hmje_async_group_create(void) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)calloc(1, sizeof(*group));
    if (group == NULL) {
        __builtin_trap();
    }
    return group;
}

void hmje_async_group_add(void *opaque_group, void *handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL || handle == NULL) {
        abort();
    }
    HmjeAsyncGroupNode *node = (HmjeAsyncGroupNode *)malloc(sizeof(*node));
    if (node == NULL) {
        __builtin_trap();
    }
    node->handle = handle;
    node->next = NULL;
    if (group->tail == NULL) {
        group->head = node;
    } else {
        group->tail->next = node;
    }
    group->tail = node;
}

int32_t hmje_async_group_poll(void *opaque_group, void *parent_handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL) {
        abort();
    }
    int32_t parent_cancelled = parent_handle != NULL && hmje_async_handle_is_cancelled(parent_handle);
    int32_t parent_failed = parent_handle != NULL && hmje_async_handle_failure(parent_handle) != 0;
    for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next) {
        if (parent_cancelled || parent_failed || group->first_failure != 0) {
            hmje_async_handle_request_cancel(node->handle);
        }
    }
    /* A child's own pending failure (for example a deadline it owns) is not
     * cancelled here: the child may still handle it with 실패하면 and continue.
     * Only a completed child's failure (first_failure below) cancels siblings.
     * Cancelling it here left the child permanently cancelled after a handled
     * timeout, so its next join silently stopped the whole task tree. */
    int32_t all_blocked = 1;
    HmjeAsyncGroupNode **link = &group->head;
    while (*link != NULL) {
        HmjeAsyncGroupNode *node = *link;
        if (!hmje_async_handle_poll_once(node->handle)) {
            if (!hmje_async_handle_is_blocked(node->handle)) {
                all_blocked = 0;
            }
            link = &node->next;
            continue;
        }
        int64_t failure = hmje_async_handle_failure(node->handle);
        if (group->first_failure == 0 && failure != 0) {
            group->first_failure = failure;
        }
        if (hmje_async_handle_is_cancelled(node->handle)) {
            group->any_cancelled = 1;
        }
        *link = node->next;
        hmje_async_handle_destroy(node->handle);
        free(node);
    }
    group->tail = NULL;
    for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next) {
        group->tail = node;
    }
    if (group->head != NULL && group->first_failure != 0) {
        for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next) {
            hmje_async_handle_request_cancel(node->handle);
        }
    }
    if (group->head == NULL) {
        return 2;
    }
    return all_blocked ? 1 : 0;
}

void hmje_async_group_finish(void *opaque_group, void *parent_handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL) {
        abort();
    }
    int64_t first_failure = group->first_failure;
    int32_t any_cancelled = group->any_cancelled;
    HmjeAsyncGroupNode *node = group->head;
    while (node != NULL) {
        HmjeAsyncGroupNode *next = node->next;
        int64_t failure = hmje_async_handle_failure(node->handle);
        if (first_failure == 0 && failure != 0) {
            first_failure = failure;
        }
        if (hmje_async_handle_is_cancelled(node->handle)) {
            any_cancelled = 1;
        }
        hmje_async_handle_destroy(node->handle);
        free(node);
        node = next;
    }
    free(group);
    if (parent_handle != NULL) {
        /* Children cancelled because the parent itself failed (a deadline
         * or an earlier failure) must not turn that catchable failure into a
         * permanent parent cancellation. */
        if (first_failure != 0) {
            hmje_async_handle_set_failure(parent_handle, first_failure);
        } else if (any_cancelled && hmje_async_handle_failure(parent_handle) == 0) {
            hmje_async_handle_request_cancel(parent_handle);
        }
    }
}

/* Diagnostics only: HMJE_ASYNC_DEBUG_WAKE_QUANTUM_MS=16 makes the root executor
 * resume completions only on a fixed quantum, reproducing a frame-locked UI
 * executor (one task tick per display frame) inside a CLI benchmark. */
static pthread_once_t hmje_async_debug_quantum_once = PTHREAD_ONCE_INIT;
static uint64_t hmje_async_debug_quantum_ms = 0;
static void hmje_async_debug_quantum_initialize(void) {
    const char *value = getenv("HMJE_ASYNC_DEBUG_WAKE_QUANTUM_MS");
    if (value != NULL) {
        hmje_async_debug_quantum_ms = (uint64_t)strtoull(value, NULL, 10);
    }
}

void hmje_async_wait_for_wake(void) {
    pthread_mutex_lock(&hmje_async_lock);
    while (hmje_async_pending_wakes == 0) {
        hmje_async_blocking_wait_count += 1;
        pthread_cond_wait(&hmje_async_wake_condition, &hmje_async_lock);
    }
    hmje_async_pending_wakes -= 1;
    pthread_mutex_unlock(&hmje_async_lock);
    pthread_once(&hmje_async_debug_quantum_once, hmje_async_debug_quantum_initialize);
    if (hmje_async_debug_quantum_ms != 0) {
        uint64_t now = hmje_async_monotonic_milliseconds();
        uint64_t next = (now / hmje_async_debug_quantum_ms + 1) * hmje_async_debug_quantum_ms;
        struct timespec request;
        request.tv_sec = (time_t)((next - now) / UINT64_C(1000));
        request.tv_nsec = (long)(((next - now) % UINT64_C(1000)) * UINT64_C(1000000));
        while (nanosleep(&request, &request) != 0) {
        }
    }
}

void hmje_async_notify_frame(void) {
    pthread_mutex_lock(&hmje_async_lock);
    hmje_async_frame_generation += 1;
    pthread_cond_broadcast(&hmje_async_wake_condition);
    pthread_mutex_unlock(&hmje_async_lock);
    hmje_async_host_wake();
}

static int32_t hmje_async_sleep_flags_locked(void) {
    int32_t flags = 0;
    if (hmje_async_wake_generation != hmje_async_wake_generation_seen) {
        hmje_async_wake_generation_seen = hmje_async_wake_generation;
        flags |= 1;
    }
    if (hmje_async_frame_generation != hmje_async_frame_generation_seen) {
        hmje_async_frame_generation_seen = hmje_async_frame_generation;
        flags |= 2;
    }
    return flags;
}

/* Returns 0 on timeout, bit 0 for an async completion, bit 1 for a display
 * refresh, bit 2 for native input/lifecycle activity, and bit 3 when the host
 * provides an event-aware native wait. */
int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms) {
    pthread_mutex_lock(&hmje_async_lock);
    {
        int32_t pending = hmje_async_sleep_flags_locked();
        if (pending != 0) {
            pthread_mutex_unlock(&hmje_async_lock);
            return pending;
        }
    }
    if (timeout_ms == 0) {
        pthread_mutex_unlock(&hmje_async_lock);
        return 0;
    }
    /* AppKit and similar UI loops must wait on their native event source so a
     * click/key/reopen wakes a truly idle app.  Async completions call the
     * paired wake hook above, closing the check-before-wait race with a queued
     * application event.  Bit 2 is reserved for native input/lifecycle wake. */
    pthread_mutex_unlock(&hmje_async_lock);
    int32_t host_wake = hmje_async_host_wait_ms(timeout_ms);
    if (host_wake >= 0) {
        pthread_mutex_lock(&hmje_async_lock);
        int32_t flags = hmje_async_sleep_flags_locked();
        pthread_mutex_unlock(&hmje_async_lock);
        /* Bit 3 advertises that this process has an event-aware native wait;
         * the shared UI loop must not enter long idle sleeps on hosts whose
         * input queue cannot wake the pthread condition fallback. */
        return flags | host_wake | 8;
    }
    pthread_mutex_lock(&hmje_async_lock);
    struct timespec deadline = hmje_async_monotonic_after(timeout_ms);
    while (hmje_async_wake_generation == hmje_async_wake_generation_seen &&
           hmje_async_frame_generation == hmje_async_frame_generation_seen) {
        int status = hmje_async_condition_wait_until(
            &hmje_async_wake_condition, &hmje_async_lock, &deadline);
        if (status == ETIMEDOUT) {
            break;
        }
    }
    int32_t woke = hmje_async_sleep_flags_locked();
    pthread_mutex_unlock(&hmje_async_lock);
    return woke;
}

uint64_t hmje_async_debug_blocking_wait_count(void) {
    pthread_mutex_lock(&hmje_async_lock);
    uint64_t count = hmje_async_blocking_wait_count;
    pthread_mutex_unlock(&hmje_async_lock);
    return count;
}
