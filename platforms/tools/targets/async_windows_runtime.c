#include "async_host_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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

static SRWLOCK hmje_async_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE hmje_async_wake_condition =
    CONDITION_VARIABLE_INIT;
static uint64_t hmje_async_next_token = 1;
static uint64_t hmje_async_pending_wakes = 0;
static uint64_t hmje_async_blocking_wait_count = 0;
static uint64_t hmje_async_wake_generation = 0;
static uint64_t hmje_async_wake_generation_seen = 0;
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
static SRWLOCK hmje_async_channel_waiter_lock = SRWLOCK_INIT;
static HmjeAsyncChannelWaiter *hmje_async_channel_waiters = NULL;

static void hmje_async_channel_lock(uint64_t channel) {
    volatile LONG64 *lock = (volatile LONG64 *)(uintptr_t)channel;
    while (InterlockedExchange64(lock, 1) != 0) {
        SwitchToThread();
    }
}

static void hmje_async_channel_unlock(uint64_t channel) {
    volatile LONG64 *lock = (volatile LONG64 *)(uintptr_t)channel;
    InterlockedExchange64(lock, 0);
}

static int hmje_async_channel_ready_locked(uint64_t channel, uint64_t operation) {
    uint8_t *base = (uint8_t *)(uintptr_t)channel;
    uint64_t capacity = *(uint64_t *)(void *)(base + 8);
    uint64_t stride = *(uint64_t *)(void *)(base + 24);
    uint64_t head = *(uint64_t *)(void *)(base + 32);
    uint64_t occupied = *(uint64_t *)(void *)(base + 48);
    uint64_t senders = *(uint64_t *)(void *)(base + 56);
    uint64_t receivers = *(uint64_t *)(void *)(base + 64);
    if (operation == 1) {
        if (senders == 0) return 1;
        if (occupied == 0 || head >= capacity) return 0;
        return *(uint64_t *)(void *)(base + 152 + head * stride) == 3;
    }
    if (operation == 2) return receivers == 0 || occupied < capacity;
    return 1;
}

void hmje_async_channel_wait(uint64_t channel, uint64_t operation, uint64_t token) {
    if (channel == 0 || (operation != 1 && operation != 2)) {
        hmje_async_complete_void(token, 22);
        return;
    }
    HmjeAsyncChannelWaiter *waiter =
        (HmjeAsyncChannelWaiter *)malloc(sizeof(HmjeAsyncChannelWaiter));
    if (waiter == NULL) abort();
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
    AcquireSRWLockExclusive(&hmje_async_channel_waiter_lock);
    waiter->next = hmje_async_channel_waiters;
    hmje_async_channel_waiters = waiter;
    ReleaseSRWLockExclusive(&hmje_async_channel_waiter_lock);
    hmje_async_channel_unlock(channel);
}

static void hmje_async_channel_wake(uint64_t channel, uint64_t operation, int destroy) {
    HmjeAsyncChannelWaiter *ready = NULL;
    AcquireSRWLockExclusive(&hmje_async_channel_waiter_lock);
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
    ReleaseSRWLockExclusive(&hmje_async_channel_waiter_lock);
    while (ready != NULL) {
        HmjeAsyncChannelWaiter *waiter = ready;
        ready = waiter->next;
        hmje_async_complete_void(waiter->token, 0);
        free(waiter);
    }
}

void hmje_async_channel_notify(uint64_t channel, uint64_t operation) {
    if (channel != 0 && operation != 0) hmje_async_channel_wake(channel, operation, 0);
}

void hmje_async_channel_destroy(uint64_t channel) {
    if (channel != 0) hmje_async_channel_wake(channel, 3, 1);
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
    CONDITION_VARIABLE done_condition;
    struct HmjeAsyncDbJob *next;
} HmjeAsyncDbJob;

typedef struct HmjeAsyncDbWorker {
    uint64_t identifier;
    HANDLE thread;
    DWORD thread_id;
    SRWLOCK lock;
    CONDITION_VARIABLE ready;
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
static SRWLOCK hmje_async_db_registry_lock = SRWLOCK_INIT;
static HmjeAsyncDbWorker *hmje_async_db_workers;
static uint64_t hmje_async_db_next_identifier = 1;
static HmjeAsyncDbTaskChild *hmje_async_db_task_children;

static void hmje_async_db_register_child(void *handle) {
    if (hmje_async_db_task_scope == NULL || handle == NULL ||
        handle == hmje_async_db_task_scope->owner) return;
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbTaskChild *child = hmje_async_db_task_children;
    while (child != NULL && child->handle != handle) child = child->next;
    if (child == NULL) {
        child = malloc(sizeof(*child));
        if (child == NULL) abort();
        child->handle = handle;
        child->next = hmje_async_db_task_children;
        hmje_async_db_task_children = child;
    }
    child->owner = hmje_async_db_task_scope->owner;
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
}

static void hmje_async_db_forget_child(void *handle) {
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbTaskChild **link = &hmje_async_db_task_children;
    while (*link != NULL && (*link)->handle != handle)
        link = &(*link)->next;
    HmjeAsyncDbTaskChild *child = *link;
    if (child != NULL) *link = child->next;
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    free(child);
}

void hmje_async_db_task_push(void *handle) {
    HmjeAsyncDbTaskScope *scope = malloc(sizeof(*scope));
    if (scope == NULL) abort();
    scope->owner = hmje_async_db_task_scope == NULL
        ? handle : hmje_async_db_task_scope->owner;
    scope->previous = hmje_async_db_task_scope;
    hmje_async_db_task_scope = scope;
}

static void hmje_async_db_task_push_root(void *handle) {
    HmjeAsyncDbTaskScope *scope = malloc(sizeof(*scope));
    if (scope == NULL) abort();
    scope->owner = handle;
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
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
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
        AcquireSRWLockExclusive(&worker->lock);
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
        WakeConditionVariable(&worker->ready);
        ReleaseSRWLockExclusive(&worker->lock);
    }
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
}

static HmjeAsyncDbWorker *hmje_async_db_find_worker_locked(uint64_t identifier) {
    for (HmjeAsyncDbWorker *worker = hmje_async_db_workers;
         worker != NULL; worker = worker->registry_next)
        if (worker->identifier == identifier) return worker;
    return NULL;
}

int32_t hmje_async_db_worker_is_live(uint64_t identifier) {
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(identifier);
    int32_t live = 0;
    if (worker != NULL) {
        AcquireSRWLockExclusive(&worker->lock);
        live = !worker->stopping;
        ReleaseSRWLockExclusive(&worker->lock);
    }
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    return live;
}

void hmje_async_db_worker_set_migration_opcode(uint64_t identifier,
                                                uint64_t opcode) {
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(identifier);
    if (worker != NULL) {
        AcquireSRWLockExclusive(&worker->lock);
        worker->migration_opcode = opcode;
        ReleaseSRWLockExclusive(&worker->lock);
    }
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
}

/* Cancellation and COMMIT start are ordered under the short worker lock. */
int32_t hmje_async_db_migration_commit_enter(void) {
    HmjeAsyncDbWorker *worker = hmje_async_db_current_worker;
    HmjeAsyncDbJob *job = hmje_async_db_current_job;
    if (worker == NULL || job == NULL ||
        job->opcode != worker->migration_opcode) return 1;
    AcquireSRWLockExclusive(&worker->lock);
    if (job->cancelled || worker->stopping) {
        ReleaseSRWLockExclusive(&worker->lock);
        return 0;
    }
    job->commit_started = 1;
    ReleaseSRWLockExclusive(&worker->lock);
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

static DWORD WINAPI hmje_async_db_worker_run(LPVOID argument) {
    HmjeAsyncDbWorker *worker = (HmjeAsyncDbWorker *)argument;
    for (;;) {
        AcquireSRWLockExclusive(&worker->lock);
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
                ReleaseSRWLockExclusive(&worker->lock);
                return 0;
            }
#ifndef NDEBUG
            if (worker->owner != NULL && worker->first != NULL) {
                uint64_t now = GetTickCount64();
                for (HmjeAsyncDbJob *held = worker->first;
                     held != NULL; held = held->next) {
                    if (!held->warned && now - held->queued_at_ms >= 5000) {
                        fprintf(stderr, "HMJE DB warning: request held >5 s behind open transaction owned by task %p at %s\n", worker->owner, worker->owner_location == NULL ? "<unknown>" : worker->owner_location);
                        held->warned = 1;
                    }
                }
                SleepConditionVariableSRW(&worker->ready, &worker->lock, 1000, 0);
            } else
#endif
                SleepConditionVariableSRW(&worker->ready, &worker->lock,
                                          INFINITE, 0);
        }
        if (job == NULL) {
            uint8_t rollback[16];
            hmje_async_db_write_u64(rollback, 1);
            hmje_async_db_write_u64(rollback + 8, worker->rollback_opcode);
            ReleaseSRWLockExclusive(&worker->lock);
            uint64_t ignored_length = 0;
            uint64_t ignored = worker->callback(
                worker->state, (uint64_t)(uintptr_t)rollback, sizeof(rollback),
                (uint64_t)(uintptr_t)&ignored_length);
            int rollback_succeeded = ignored != 0 && ignored_length >= 16 &&
                hmje_async_db_read_u64((const uint8_t *)(uintptr_t)ignored) == 1 &&
                hmje_async_db_read_u64((const uint8_t *)(uintptr_t)ignored + 8) != 0;
            free((void *)(uintptr_t)ignored);
            if (!rollback_succeeded) {
                uint8_t close_request[16];
                hmje_async_db_write_u64(close_request, 1);
                hmje_async_db_write_u64(close_request + 8, 3);
                uint64_t close_length = 0;
                uint64_t close_result = worker->callback(
                    worker->state, (uint64_t)(uintptr_t)close_request,
                    sizeof(close_request), (uint64_t)(uintptr_t)&close_length);
                free((void *)(uintptr_t)close_result);
            }
            AcquireSRWLockExclusive(&worker->lock);
            worker->owner = NULL;
            free(worker->owner_location);
            worker->owner_location = NULL;
            worker->transaction_depth = 0;
            worker->owner_cancelled = 0;
            WakeConditionVariable(&worker->ready);
            ReleaseSRWLockExclusive(&worker->lock);
            continue;
        }
        int cancelled_before_run = job->cancelled;
        ReleaseSRWLockExclusive(&worker->lock);

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
        AcquireSRWLockExclusive(&worker->lock);
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
        WakeConditionVariable(&worker->ready);
        ReleaseSRWLockExclusive(&worker->lock);
        free(job->source_location);
        HmjeAsyncDbResult *result = malloc(sizeof(*result));
        if (cancelled_before_run || result == NULL ||
            (result_bytes == 0 && result_length != 0)) {
            free((void *)(uintptr_t)result_bytes);
            free(result);
            job->status = cancelled_before_run
                ? ERROR_OPERATION_ABORTED : ERROR_NOT_ENOUGH_MEMORY;
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
            AcquireSRWLockExclusive(&worker->lock);
            job->done = 1;
            WakeConditionVariable(&job->done_condition);
            ReleaseSRWLockExclusive(&worker->lock);
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
    InitializeSRWLock(&worker->lock);
    InitializeConditionVariable(&worker->ready);
    worker->thread = CreateThread(NULL, 0, hmje_async_db_worker_run,
                                  worker, 0, &worker->thread_id);
    if (worker->thread == NULL) {
        free(worker);
        return 0;
    }
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    worker->identifier = hmje_async_db_next_identifier++;
    if (hmje_async_db_next_identifier == 0) abort();
    worker->registry_next = hmje_async_db_workers;
    hmje_async_db_workers = worker;
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
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
        AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
        HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
        AcquireSRWLockExclusive(&worker->lock);
        worker->begin_opcode = begin_opcode;
        worker->commit_opcode = commit_opcode;
        worker->rollback_opcode = rollback_opcode;
        ReleaseSRWLockExclusive(&worker->lock);
        ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    }
    return handle;
}

static int32_t hmje_async_db_worker_enqueue(
    uint64_t handle, uint64_t request, uint64_t request_length,
    uint64_t token, HmjeAsyncDbJob **out_job,
    HmjeAsyncDbWorker **out_worker) {
    if (handle == 0 || (request == 0 && request_length != 0) ||
        request_length > SIZE_MAX) return ERROR_INVALID_PARAMETER;
    HmjeAsyncDbJob *job = calloc(1, sizeof(*job));
    if (job == NULL) return ERROR_NOT_ENOUGH_MEMORY;
    if (request_length != 0) {
        job->request = malloc((size_t)request_length);
        if (job->request == NULL) {
            free(job);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        memcpy(job->request, (const void *)(uintptr_t)request,
               (size_t)request_length);
    }
    job->request_length = request_length;
    job->token = token;
    job->owner = hmje_async_db_task_scope == NULL
        ? NULL : hmje_async_db_task_scope->owner;
    job->queued_at_ms = GetTickCount64();
    InitializeConditionVariable(&job->done_condition);
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
    if (worker == NULL) {
        ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
        free(job->request);
        free(job);
        return ERROR_INVALID_HANDLE;
    }
    AcquireSRWLockExclusive(&worker->lock);
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    if (worker->stopping) {
        ReleaseSRWLockExclusive(&worker->lock);
        free(job->request);
        free(job->source_location);
        free(job);
        return ERROR_OPERATION_ABORTED;
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
    WakeConditionVariable(&worker->ready);
    ReleaseSRWLockExclusive(&worker->lock);
    *out_job = job;
    if (out_worker != NULL) *out_worker = worker;
    return 0;
}

int32_t hmje_async_db_worker_submit(uint64_t worker, uint64_t request,
                                    uint64_t request_length, uint64_t token) {
    if (token == 0) return ERROR_INVALID_PARAMETER;
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
    if (result_address == 0) return ERROR_INVALID_PARAMETER;
    *(uint64_t *)(uintptr_t)result_address = 0;
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *target = hmje_async_db_find_worker_locked(worker);
    if (target != NULL && GetCurrentThreadId() == target->thread_id) {
        ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
        return ERROR_INVALID_FUNCTION;
    }
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbJob *job = NULL;
    int32_t status = hmje_async_db_worker_enqueue(
        worker, request, request_length, 0, &job, &target);
    if (status != 0) return status;
    AcquireSRWLockExclusive(&target->lock);
    while (!job->done) {
        SleepConditionVariableSRW(&job->done_condition, &target->lock,
                                  INFINITE, 0);
    }
    target->sync_waiters -= 1;
    WakeConditionVariable(&target->ready);
    ReleaseSRWLockExclusive(&target->lock);
    *(uint64_t *)(uintptr_t)result_address =
        (uint64_t)(uintptr_t)job->result;
    status = job->status;
    free(job);
    return status;
}

void hmje_async_db_worker_destroy(uint64_t handle) {
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker *worker = hmje_async_db_find_worker_locked(handle);
    if (worker == NULL || GetCurrentThreadId() == worker->thread_id) {
        ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
        return;
    }
    AcquireSRWLockExclusive(&worker->lock);
    if (worker->stopping) {
        ReleaseSRWLockExclusive(&worker->lock);
        ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
        return;
    }
    worker->stopping = 1;
    WakeConditionVariable(&worker->ready);
    ReleaseSRWLockExclusive(&worker->lock);
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    WaitForSingleObject(worker->thread, INFINITE);
    AcquireSRWLockExclusive(&worker->lock);
    while (worker->sync_waiters != 0)
        SleepConditionVariableSRW(&worker->ready, &worker->lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&worker->lock);
    AcquireSRWLockExclusive(&hmje_async_db_registry_lock);
    HmjeAsyncDbWorker **link = &hmje_async_db_workers;
    while (*link != NULL && *link != worker) link = &(*link)->registry_next;
    if (*link == worker) *link = worker->registry_next;
    ReleaseSRWLockExclusive(&hmje_async_db_registry_lock);
    /* Submitters acquire worker->lock under the registry lock, then release
     * the registry. After unlinking, drain the last such critical section. */
    AcquireSRWLockExclusive(&worker->lock);
    ReleaseSRWLockExclusive(&worker->lock);
    CloseHandle(worker->thread);
    free(worker->owner_location);
    free(worker);
}

typedef struct HmjeAsyncDbClose {
    uint64_t worker;
    uint64_t token;
} HmjeAsyncDbClose;

static DWORD WINAPI hmje_async_db_worker_reap(LPVOID argument) {
    HmjeAsyncDbClose *close_request = (HmjeAsyncDbClose *)argument;
    hmje_async_db_worker_destroy(close_request->worker);
    hmje_async_complete_void(close_request->token, 0);
    free(close_request);
    return 0;
}

void hmje_async_db_worker_destroy_async(uint64_t worker, uint64_t token) {
    HmjeAsyncDbClose *close_request = malloc(sizeof(*close_request));
    if (close_request == NULL) {
        hmje_async_complete_void(token, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    close_request->worker = worker;
    close_request->token = token;
    HANDLE thread = CreateThread(NULL, 0, hmje_async_db_worker_reap,
                                 close_request, 0, NULL);
    if (thread == NULL) {
        free(close_request);
        hmje_async_complete_void(token, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    CloseHandle(thread);
}

static SRWLOCK hmje_async_timer_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE hmje_async_timer_condition =
    CONDITION_VARIABLE_INIT;
static HmjeAsyncTimer *hmje_async_timers = NULL;
static int32_t hmje_async_timer_started = 0;
static int32_t hmje_async_timer_start_status = 0;
static uint64_t hmje_async_next_deadline_token = 1;

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
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = calloc(1, sizeof(*state));
        if (state == NULL) abort();
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->generation == 0) {
        state->generation = hmje_async_next_handle_generation++;
        if (hmje_async_next_handle_generation == 0) abort();
    }
    uint64_t generation = state->generation;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return generation;
}

static void hmje_async_deadline_publish_failure(void *handle, uint64_t generation) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    /* Never recreate forgotten state or tag a replacement frame. */
    if (state != NULL && state->generation == generation && state->failure == 0) {
        state->failure = -110;
        hmje_async_pending_wakes += 1;
        hmje_async_wake_generation += 1;
        WakeConditionVariable(&hmje_async_wake_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
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
    AcquireSRWLockExclusive(&hmje_async_lock);
    limit->identifier = hmje_async_next_nonzero_identifier(
        &hmje_async_next_limit_identifier);
    limit->capacity = capacity;
    limit->next = hmje_async_limits;
    hmje_async_limits = limit;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return limit->identifier;
}

void hmje_async_limit_acquire(uint64_t identifier, uint64_t token) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncLimit *limit = hmje_async_find_limit_locked(identifier);
    if (limit == NULL) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        hmje_async_complete_void(token, 22);
        return;
    }
    if (limit->in_use < limit->capacity) {
        HmjeAsyncPermit *permit = hmje_async_permit_create_locked(limit);
        ReleaseSRWLockExclusive(&hmje_async_lock);
        if (permit == NULL) {
            hmje_async_complete_void(token, 12);
            return;
        }
        hmje_async_complete_owned_u64(
            token, 0, permit->identifier, hmje_async_permit_cleanup);
        return;
    }
    HmjeAsyncLimitWaiter *waiter =
        (HmjeAsyncLimitWaiter *)malloc(sizeof(HmjeAsyncLimitWaiter));
    if (waiter == NULL) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        hmje_async_complete_void(token, 12);
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
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

void hmje_async_permit_release(uint64_t identifier) {
    uint64_t grant_token = 0;
    uint64_t grant_identifier = 0;
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncPermit **permit_link = &hmje_async_permits;
    while (*permit_link != NULL && (*permit_link)->identifier != identifier) {
        permit_link = &(*permit_link)->next;
    }
    HmjeAsyncPermit *permit = *permit_link;
    if (permit == NULL || permit->limit == NULL || permit->limit->in_use == 0) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
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
    ReleaseSRWLockExclusive(&hmje_async_lock);
    if (grant_token != 0) {
        hmje_async_complete_owned_u64(
            grant_token,
            0,
            grant_identifier,
            hmje_async_permit_cleanup);
    }
}

int64_t hmje_async_limit_destroy(uint64_t identifier) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncLimit **link = &hmje_async_limits;
    while (*link != NULL && (*link)->identifier != identifier) {
        link = &(*link)->next;
    }
    HmjeAsyncLimit *limit = *link;
    if (limit == NULL) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        return -22;
    }
    if (limit->in_use != 0 || limit->wait_head != NULL) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        return -16;
    }
    *link = limit->next;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    free(limit);
    return 0;
}

static DWORD WINAPI hmje_async_timer_worker(void *unused) {
    (void)unused;
    for (;;) {
        AcquireSRWLockExclusive(&hmje_async_timer_lock);
        while (hmje_async_timers == NULL) {
            SleepConditionVariableSRW(
                &hmje_async_timer_condition,
                &hmje_async_timer_lock,
                INFINITE,
                0);
        }

        uint64_t now = GetTickCount64();
        HmjeAsyncTimer *timer = hmje_async_timers;
        if (timer->deadline_milliseconds > now) {
            uint64_t remaining = timer->deadline_milliseconds - now;
            DWORD delay = remaining >= (uint64_t)(INFINITE - 1)
                ? INFINITE - 1
                : (DWORD)remaining;
            SleepConditionVariableSRW(
                &hmje_async_timer_condition,
                &hmje_async_timer_lock,
                delay,
                0);
            ReleaseSRWLockExclusive(&hmje_async_timer_lock);
            continue;
        }

        hmje_async_timers = timer->next;
        if (timer->kind == 1) {
            /* Cancellation can synchronously unwind the deadline scope and
             * reacquire this lock through deadline_cancel_for_handle. */
            void *handle = timer->handle;
            ReleaseSRWLockExclusive(&hmje_async_timer_lock);
            hmje_async_deadline_publish_failure(handle, timer->handle_generation);
        } else {
            ReleaseSRWLockExclusive(&hmje_async_timer_lock);
            hmje_async_complete_void(timer->token, 0);
        }
        free(timer);
    }
}

void hmje_async_timer_wait(uint64_t delay_milliseconds, uint64_t token) {
    HmjeAsyncTimer *timer = (HmjeAsyncTimer *)malloc(sizeof(HmjeAsyncTimer));
    if (timer == NULL) {
        hmje_async_complete_void(token, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    AcquireSRWLockExclusive(&hmje_async_timer_lock);
    if (!hmje_async_timer_started && hmje_async_timer_start_status == 0) {
        HANDLE worker = CreateThread(
            NULL, 0, hmje_async_timer_worker, NULL, 0, NULL);
        if (worker == NULL) {
            hmje_async_timer_start_status = (int32_t)GetLastError();
        } else {
            hmje_async_timer_started = 1;
            CloseHandle(worker);
        }
    }
    if (hmje_async_timer_start_status != 0) {
        int32_t status = hmje_async_timer_start_status;
        ReleaseSRWLockExclusive(&hmje_async_timer_lock);
        free(timer);
        hmje_async_complete_void(token, status);
        return;
    }

    uint64_t now = GetTickCount64();
    timer->deadline_milliseconds = delay_milliseconds > UINT64_MAX - now
        ? UINT64_MAX
        : now + delay_milliseconds;
    timer->token = token;
    timer->handle = NULL;
    timer->kind = 0;
    timer->next = NULL;

    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL &&
           (*link)->deadline_milliseconds <= timer->deadline_milliseconds) {
        link = &(*link)->next;
    }
    int wake_worker = link == &hmje_async_timers;
    timer->next = *link;
    *link = timer;
    if (wake_worker) {
        WakeConditionVariable(&hmje_async_timer_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_timer_lock);
}

uint64_t hmje_async_deadline_begin(uint64_t delay_milliseconds, void *handle) {
    if (handle == NULL) {
        abort();
    }
    HmjeAsyncTimer *timer = (HmjeAsyncTimer *)malloc(sizeof(HmjeAsyncTimer));
    if (timer == NULL) {
        abort();
    }
    AcquireSRWLockExclusive(&hmje_async_timer_lock);
    if (!hmje_async_timer_started && hmje_async_timer_start_status == 0) {
        HANDLE worker = CreateThread(NULL, 0, hmje_async_timer_worker, NULL, 0, NULL);
        if (worker == NULL) {
            hmje_async_timer_start_status = (int32_t)GetLastError();
        } else {
            hmje_async_timer_started = 1;
            CloseHandle(worker);
        }
    }
    if (hmje_async_timer_start_status != 0) {
        ReleaseSRWLockExclusive(&hmje_async_timer_lock);
        free(timer);
        abort();
    }
    uint64_t now = GetTickCount64();
    timer->deadline_milliseconds = delay_milliseconds > UINT64_MAX - now
        ? UINT64_MAX
        : now + delay_milliseconds;
    timer->token = hmje_async_next_deadline_token++;
    if (timer->token == 0) {
        timer->token = hmje_async_next_deadline_token++;
    }
    uint64_t deadline_token = timer->token;
    timer->handle = handle;
    timer->handle_generation = hmje_async_deadline_handle_generation(handle);
    timer->kind = 1;
    timer->next = NULL;
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL && (*link)->deadline_milliseconds <= timer->deadline_milliseconds) {
        link = &(*link)->next;
    }
    int wake_worker = link == &hmje_async_timers;
    timer->next = *link;
    *link = timer;
    if (wake_worker) {
        WakeConditionVariable(&hmje_async_timer_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_timer_lock);
    return deadline_token;
}

void hmje_async_deadline_end(uint64_t deadline_token) {
    AcquireSRWLockExclusive(&hmje_async_timer_lock);
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL) {
        HmjeAsyncTimer *timer = *link;
        if (timer->kind == 1 && timer->token == deadline_token) {
            *link = timer->next;
            free(timer);
            WakeConditionVariable(&hmje_async_timer_condition);
            break;
        }
        link = &timer->next;
    }
    ReleaseSRWLockExclusive(&hmje_async_timer_lock);
}

void hmje_async_deadline_cancel_for_handle(void *handle) {
    AcquireSRWLockExclusive(&hmje_async_timer_lock);
    HmjeAsyncTimer **link = &hmje_async_timers;
    while (*link != NULL) {
        HmjeAsyncTimer *timer = *link;
        if (timer->kind == 1 && timer->handle == handle) {
            *link = timer->next;
            free(timer);
            continue;
        }
        link = &timer->next;
    }
    WakeConditionVariable(&hmje_async_timer_condition);
    ReleaseSRWLockExclusive(&hmje_async_timer_lock);
}

uint64_t hmje_async_event_create(void) {
    HmjeAsyncEvent *event = (HmjeAsyncEvent *)calloc(1, sizeof(HmjeAsyncEvent));
    if (event == NULL) {
        abort();
    }
    AcquireSRWLockExclusive(&hmje_async_lock);
    event->token = hmje_async_next_token++;
    if (event->token == 0) {
        event->token = hmje_async_next_token++;
    }
    event->next = hmje_async_events;
    hmje_async_events = event;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return event->token;
}

void hmje_async_complete_owned_u64(
    uint64_t token,
    int32_t status,
    uint64_t value,
    void (*cleanup)(uint64_t)) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL || event->ready) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        abort();
    }
    if (event->abandoned) {
        *link = event->next;
        ReleaseSRWLockExclusive(&hmje_async_lock);
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
    WakeConditionVariable(&hmje_async_wake_condition);
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value) {
    hmje_async_complete_owned_u64(token, status, value, NULL);
}

void hmje_async_complete_void(uint64_t token, int32_t status) {
    hmje_async_complete_u64(token, status, 0);
}

int32_t hmje_async_event_is_ready(uint64_t token) {
    AcquireSRWLockShared(&hmje_async_lock);
    HmjeAsyncEvent *event = hmje_async_find_event_locked(token);
    int32_t ready = event != NULL && event->ready;
    ReleaseSRWLockShared(&hmje_async_lock);
    return ready;
}

int32_t hmje_async_event_take_u64(
    uint64_t token,
    uint64_t *value,
    int32_t *status) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL || !event->ready) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
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
    ReleaseSRWLockExclusive(&hmje_async_lock);
    free(event);
    return 1;
}

int32_t hmje_async_event_abandon(uint64_t token) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncEvent **link = &hmje_async_events;
    while (*link != NULL && (*link)->token != token) {
        link = &(*link)->next;
    }
    HmjeAsyncEvent *event = *link;
    if (event == NULL) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        return 0;
    }
    if (event->ready) {
        *link = event->next;
        if (hmje_async_pending_wakes > 0) {
            hmje_async_pending_wakes -= 1;
        }
        ReleaseSRWLockExclusive(&hmje_async_lock);
        if (event->cleanup != NULL) {
            event->cleanup(event->value);
        }
        free(event);
        return 1;
    }
    event->abandoned = 1;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return 1;
}

int32_t hmje_async_event_is_abandoned(uint64_t token) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncEvent *event = hmje_async_find_event_locked(token);
    int32_t abandoned = event != NULL && event->abandoned;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return abandoned;
}

void hmje_async_handle_set_blocked(void *handle, int32_t blocked) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            ReleaseSRWLockExclusive(&hmje_async_lock);
            abort();
        }
        state->handle = handle;
        state->cancelled = 0;
        state->cancellation_shield_depth = 0;
        state->failure = 0;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    state->blocked = blocked != 0;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    if (blocked) hmje_async_db_register_child(handle);
}

int32_t hmje_async_handle_is_blocked(void *handle) {
    AcquireSRWLockShared(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int32_t blocked = state != NULL && state->blocked;
    ReleaseSRWLockShared(&hmje_async_lock);
    return blocked;
}

void hmje_async_handle_request_cancel(void *handle) {
    if (handle == NULL) {
        return;
    }
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            ReleaseSRWLockExclusive(&hmje_async_lock);
            abort();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (!state->cancelled) {
        state->cancelled = 1;
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        WakeConditionVariable(&hmje_async_wake_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
    hmje_async_db_cancel_owner(handle, 1);
}

void hmje_async_handle_request_cancel_address(uint64_t handle_address) {
    hmje_async_handle_request_cancel((void *)(uintptr_t)handle_address);
}

int32_t hmje_async_handle_is_cancelled(void *handle) {
    AcquireSRWLockShared(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int32_t cancelled = state != NULL && state->cancelled &&
        state->cancellation_shield_depth == 0;
    ReleaseSRWLockShared(&hmje_async_lock);
    return cancelled;
}

void hmje_async_cancellation_shield_enter(void *handle) {
    if (handle == NULL) abort();
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            ReleaseSRWLockExclusive(&hmje_async_lock);
            abort();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->cancellation_shield_depth == UINT32_MAX) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        abort();
    }
    state->cancellation_shield_depth += 1;
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

void hmje_async_cancellation_shield_exit(void *handle) {
    if (handle == NULL) abort();
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL || state->cancellation_shield_depth == 0) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        abort();
    }
    state->cancellation_shield_depth -= 1;
    if (state->cancellation_shield_depth == 0 && state->cancelled) {
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        WakeConditionVariable(&hmje_async_wake_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

void hmje_async_handle_set_failure(void *handle, int64_t status) {
    if (handle == NULL || status == 0) {
        return;
    }
    if (status > 0) {
        status = -status;
    }
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    if (state == NULL) {
        state = (HmjeAsyncHandleState *)calloc(1, sizeof(HmjeAsyncHandleState));
        if (state == NULL) {
            ReleaseSRWLockExclusive(&hmje_async_lock);
            abort();
        }
        state->handle = handle;
        state->next = hmje_async_handles;
        hmje_async_handles = state;
    }
    if (state->failure == 0) {
        state->failure = status;
        hmje_async_pending_wakes += 1;
    hmje_async_wake_generation += 1;
        WakeConditionVariable(&hmje_async_wake_condition);
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

int64_t hmje_async_handle_failure(void *handle) {
    AcquireSRWLockShared(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int64_t failure = state != NULL ? state->failure : 0;
    ReleaseSRWLockShared(&hmje_async_lock);
    return failure;
}

int64_t hmje_async_handle_take_failure(void *handle) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState *state = hmje_async_find_handle_locked(handle);
    int64_t failure = state != NULL ? state->failure : 0;
    if (state != NULL) {
        state->failure = 0;
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return failure;
}

void hmje_async_handle_forget(void *handle) {
    hmje_async_db_cancel_owner(handle, 0);
    hmje_async_db_forget_child(handle);
    AcquireSRWLockExclusive(&hmje_async_lock);
    HmjeAsyncHandleState **link = &hmje_async_handles;
    while (*link != NULL && (*link)->handle != handle) {
        link = &(*link)->next;
    }
    HmjeAsyncHandleState *state = *link;
    if (state != NULL) {
        *link = state->next;
    }
    ReleaseSRWLockExclusive(&hmje_async_lock);
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
    hmje_async_db_task_push_root(handle);
    frame->resume(handle);
    hmje_async_db_task_pop();
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
    if (node->closure_release != NULL && node->closure_context != 0)
        node->closure_release(node->closure_context);
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
    if (handle == NULL) abort();
    HmjeAsyncUnstructuredNode *node =
        (HmjeAsyncUnstructuredNode *)calloc(1, sizeof(*node));
    if (node == NULL) abort();
    node->handle = handle;
    node->closure_context = closure_context;
    node->closure_release =
        (HmjeAsyncClosureRelease)(uintptr_t)closure_release_address;
    /* Eager start, as in async_host_runtime.c: run to the first suspension
     * before publishing the task to the executor queue. */
    if (hmje_async_handle_poll_once(handle)) {
        hmje_async_unstructured_release_node(node);
        return;
    }
    if (!hmje_async_unstructured_atexit_registered) {
        if (atexit(hmje_async_unstructured_shutdown) != 0) abort();
        hmje_async_unstructured_atexit_registered = 1;
    }
    if (hmje_async_unstructured_tail == NULL)
        hmje_async_unstructured_head = node;
    else
        hmje_async_unstructured_tail->next = node;
    hmje_async_unstructured_tail = node;
}

int32_t hmje_async_unstructured_poll(void) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    hmje_async_wake_generation_seen = hmje_async_wake_generation;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    int32_t all_blocked = 1;
    HmjeAsyncUnstructuredNode **link = &hmje_async_unstructured_head;
    while (*link != NULL) {
        HmjeAsyncUnstructuredNode *node = *link;
        if (!hmje_async_handle_poll_once(node->handle)) {
            if (!hmje_async_handle_is_blocked(node->handle)) all_blocked = 0;
            link = &node->next;
            continue;
        }
        *link = node->next;
        hmje_async_unstructured_release_node(node);
    }
    hmje_async_unstructured_tail = NULL;
    for (HmjeAsyncUnstructuredNode *node = hmje_async_unstructured_head;
         node != NULL; node = node->next)
        hmje_async_unstructured_tail = node;
    if (hmje_async_unstructured_head == NULL) return 2;
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
    if (group == NULL) abort();
    return group;
}

void hmje_async_group_add(void *opaque_group, void *handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL || handle == NULL) abort();
    HmjeAsyncGroupNode *node = (HmjeAsyncGroupNode *)malloc(sizeof(*node));
    if (node == NULL) abort();
    node->handle = handle;
    node->next = NULL;
    if (group->tail == NULL) group->head = node;
    else group->tail->next = node;
    group->tail = node;
}

int32_t hmje_async_group_poll(void *opaque_group, void *parent_handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL) abort();
    int32_t parent_cancelled = parent_handle != NULL && hmje_async_handle_is_cancelled(parent_handle);
    int32_t parent_failed = parent_handle != NULL && hmje_async_handle_failure(parent_handle) != 0;
    for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next) {
        if (parent_cancelled || parent_failed || group->first_failure != 0)
            hmje_async_handle_request_cancel(node->handle);
    }
    /* See async_host_runtime.c: a child's pending failure is its own to handle. */
    int32_t all_blocked = 1;
    HmjeAsyncGroupNode **link = &group->head;
    while (*link != NULL) {
        HmjeAsyncGroupNode *node = *link;
        if (!hmje_async_handle_poll_once(node->handle)) {
            if (!hmje_async_handle_is_blocked(node->handle)) all_blocked = 0;
            link = &node->next;
            continue;
        }
        int64_t failure = hmje_async_handle_failure(node->handle);
        if (group->first_failure == 0 && failure != 0)
            group->first_failure = failure;
        if (hmje_async_handle_is_cancelled(node->handle))
            group->any_cancelled = 1;
        *link = node->next;
        hmje_async_handle_destroy(node->handle);
        free(node);
    }
    group->tail = NULL;
    for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next)
        group->tail = node;
    if (group->head != NULL && group->first_failure != 0) {
        for (HmjeAsyncGroupNode *node = group->head; node != NULL; node = node->next)
            hmje_async_handle_request_cancel(node->handle);
    }
    if (group->head == NULL) return 2;
    return all_blocked ? 1 : 0;
}

void hmje_async_group_finish(void *opaque_group, void *parent_handle) {
    HmjeAsyncGroup *group = (HmjeAsyncGroup *)opaque_group;
    if (group == NULL) abort();
    int64_t first_failure = group->first_failure;
    int32_t any_cancelled = group->any_cancelled;
    HmjeAsyncGroupNode *node = group->head;
    while (node != NULL) {
        HmjeAsyncGroupNode *next = node->next;
        int64_t failure = hmje_async_handle_failure(node->handle);
        if (first_failure == 0 && failure != 0) first_failure = failure;
        if (hmje_async_handle_is_cancelled(node->handle)) any_cancelled = 1;
        hmje_async_handle_destroy(node->handle);
        free(node);
        node = next;
    }
    free(group);
    if (parent_handle != NULL) {
        if (first_failure != 0) hmje_async_handle_set_failure(parent_handle, first_failure);
        else if (any_cancelled && hmje_async_handle_failure(parent_handle) == 0)
            hmje_async_handle_request_cancel(parent_handle);
    }
}

void hmje_async_wait_for_wake(void) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    while (hmje_async_pending_wakes == 0) {
        hmje_async_blocking_wait_count += 1;
        SleepConditionVariableSRW(
            &hmje_async_wake_condition, &hmje_async_lock, INFINITE, 0);
    }
    hmje_async_pending_wakes -= 1;
    ReleaseSRWLockExclusive(&hmje_async_lock);
}

int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms) {
    AcquireSRWLockExclusive(&hmje_async_lock);
    if (hmje_async_wake_generation != hmje_async_wake_generation_seen) {
        hmje_async_wake_generation_seen = hmje_async_wake_generation;
        ReleaseSRWLockExclusive(&hmje_async_lock);
        return 1;
    }
    if (timeout_ms == 0) {
        ReleaseSRWLockExclusive(&hmje_async_lock);
        return 0;
    }
    uint64_t deadline = GetTickCount64() + timeout_ms;
    while (hmje_async_wake_generation == hmje_async_wake_generation_seen) {
        uint64_t now = GetTickCount64();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        if (remaining > 0xfffffffeULL) remaining = 0xfffffffeULL;
        if (!SleepConditionVariableSRW(
                &hmje_async_wake_condition, &hmje_async_lock, (DWORD)remaining, 0)) {
            if (GetLastError() == ERROR_TIMEOUT) break;
        }
    }
    int32_t woke = hmje_async_wake_generation != hmje_async_wake_generation_seen;
    if (woke) hmje_async_wake_generation_seen = hmje_async_wake_generation;
    ReleaseSRWLockExclusive(&hmje_async_lock);
    return woke;
}

uint64_t hmje_async_debug_blocking_wait_count(void) {
    AcquireSRWLockShared(&hmje_async_lock);
    uint64_t count = hmje_async_blocking_wait_count;
    ReleaseSRWLockShared(&hmje_async_lock);
    return count;
}

int64_t hmje_host_process_wait_io(int64_t child_id, int64_t descriptor);
typedef struct {
    int64_t child_id, descriptor;
    uint64_t token;
} HmjeProcessIOWait;
static DWORD WINAPI hmje_process_io_wait_worker(void *pointer) {
    HmjeProcessIOWait *request = pointer;
    int64_t ready = hmje_host_process_wait_io(request->child_id, request->descriptor);
    hmje_async_complete_u64(request->token, ready < 0 ? (int32_t)-ready : 0,
                            ready < 0 ? 0 : (uint64_t)ready);
    free(request);
    return 0;
}
void hmje_host_process_wait_io_async(int64_t child_id, int64_t descriptor, uint64_t token) {
    HmjeProcessIOWait *request = malloc(sizeof(*request));
    if (!request) { hmje_async_complete_u64(token, 12, 0); return; }
    request->child_id = child_id; request->descriptor = descriptor; request->token = token;
    HANDLE worker = CreateThread(NULL, 0, hmje_process_io_wait_worker, request, 0, NULL);
    if (!worker) { free(request); hmje_async_complete_u64(token, 5, 0); return; }
    CloseHandle(worker);
}
