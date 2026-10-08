#ifndef HMJE_ASYNC_HOST_RUNTIME_H
#define HMJE_ASYNC_HOST_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native async adapters receive an opaque token as their last argument.  They
 * must return promptly and complete exactly once from any host thread.  A
 * callback only publishes POD completion data and wakes the executor; it must
 * never call llvm.coro.resume (or any HMJE function) itself.
 */
void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
void hmje_async_complete_void(uint64_t token, int32_t status);
/* 값이 OS resource handle일 때는 소유 완료를 사용한다. take 전 취소되면 runtime이
 * cleanup(value)을 정확히 한 번 호출하고, 정상 take면 소유권이 coroutine으로 이동한다. */
void hmje_async_complete_owned_u64(
    uint64_t token,
    int32_t status,
    uint64_t value,
    void (*cleanup)(uint64_t));

/* Built-in host timer adapter.  This has the exact native-start shape emitted
 * for `비동기 ... 외부에서 "hmje_async_timer_wait"라는 이름으로
 * 구현된다`: POD arguments followed by one completion token.  It only
 * schedules a completion event; it never resumes a coroutine itself.
 */
void hmje_async_timer_wait(uint64_t delay_milliseconds, uint64_t token);
void hmje_async_channel_wait(uint64_t channel, uint64_t operation, uint64_t token);
void hmje_async_channel_notify(uint64_t channel, uint64_t operation);
void hmje_async_channel_destroy(uint64_t channel);
/* A connection worker accepts copied bytes and returns an owned byte result.
 * The callback runs only on its dedicated worker thread.  It must allocate
 * its result with malloc; the runtime frees abandoned results.  No HMJE owner
 * pointer is passed between threads.  HMJE callers must enable the shared
 * allocator (`메모리멀티스레드켜기`) before creating a worker whose callback
 * decodes owner values on that worker. */
typedef uint64_t (*HmjeAsyncDbBytesCallback)(
    uint64_t state, uint64_t request, uint64_t request_length,
    uint64_t result_length_address);
uint64_t hmje_async_db_worker_create(uint64_t state,
                                     HmjeAsyncDbBytesCallback callback);
/* Transaction opcodes are the second little-endian u64 in the wire request.
 * A successful transaction response has a true u64 immediately after its
 * version.  The worker uses these bytes only to manage its owner gate. */
uint64_t hmje_async_db_worker_create_transactional(
    uint64_t state, HmjeAsyncDbBytesCallback callback,
    uint64_t begin_opcode, uint64_t commit_opcode, uint64_t rollback_opcode);
void hmje_async_db_task_push(void *handle);
void hmje_async_db_task_pop(void);
int32_t hmje_async_db_worker_is_live(uint64_t worker);
void hmje_async_db_worker_set_migration_opcode(uint64_t worker,
                                                uint64_t opcode);
int32_t hmje_async_db_migration_commit_enter(void);
int32_t hmje_async_db_worker_submit(uint64_t worker, uint64_t request,
                                    uint64_t request_length, uint64_t token);
/* Native async-start shape: the compiler adds token after the POD arguments. */
void hmje_async_db_worker_execute(uint64_t worker, uint64_t request,
                                  uint64_t request_length, uint64_t token);
int32_t hmje_async_db_worker_submit_sync(uint64_t worker, uint64_t request,
                                         uint64_t request_length,
                                         uint64_t result_address);
uint64_t hmje_async_db_result_data(uint64_t result);
uint64_t hmje_async_db_result_length(uint64_t result);
void hmje_async_db_result_free(uint64_t result);
/* Closing drains already queued jobs.  The caller must publish the closed
 * connection before invoking either close entry point; subsequent submits are
 * invalid.  The async form joins on a helper, never on the executor thread. */
void hmje_async_db_worker_destroy(uint64_t worker);
void hmje_async_db_worker_destroy_async(uint64_t worker, uint64_t token);
/* The encoder uses this allocator so the worker can free request/response
 * bytes without touching the HMJE ARC allocator.  A failed resize leaves the
 * old allocation valid. */
uint64_t hmje_async_db_bytes_resize(uint64_t bytes, uint64_t size);
void hmje_async_db_bytes_free(uint64_t bytes);
uint64_t hmje_async_limit_create(uint64_t capacity);
void hmje_async_limit_acquire(uint64_t limit, uint64_t token);
void hmje_async_permit_release(uint64_t permit);
int64_t hmje_async_limit_destroy(uint64_t limit);
uint64_t hmje_async_deadline_begin(uint64_t delay_milliseconds, void *handle);
void hmje_async_deadline_end(uint64_t deadline_token);
void hmje_async_deadline_cancel_for_handle(void *handle);

/* Compiler/runtime ABI.  Native project code should only use the two complete
 * functions above; these declarations live here so the runtime has one exact
 * contract and tests can audit it.
 */
uint64_t hmje_async_event_create(void);
int32_t hmje_async_event_is_ready(uint64_t token);
int32_t hmje_async_event_take_u64(uint64_t token, uint64_t *value, int32_t *status);
int32_t hmje_async_event_abandon(uint64_t token);
/* Native pollers may stop an in-flight OS operation promptly after the
 * coroutine abandons its event.  This is observation only; completion still
 * happens exactly once so the runtime can release any owned result. */
int32_t hmje_async_event_is_abandoned(uint64_t token);
void hmje_async_handle_set_blocked(void *handle, int32_t blocked);
int32_t hmje_async_handle_is_blocked(void *handle);
void hmje_async_handle_request_cancel(void *handle);
void hmje_async_handle_request_cancel_address(uint64_t handle_address);
int32_t hmje_async_handle_is_cancelled(void *handle);
void hmje_async_cancellation_shield_enter(void *handle);
void hmje_async_cancellation_shield_exit(void *handle);
void hmje_async_handle_set_failure(void *handle, int64_t status);
int64_t hmje_async_handle_failure(void *handle);
int64_t hmje_async_handle_take_failure(void *handle);
void hmje_async_handle_forget(void *handle);
/* HmjeUI's compiler-owned structured task driver uses these opaque-handle
 * operations.  They are runtime SPI, never a source-level task handle API. */
int32_t hmje_async_handle_poll_once(void *handle);
void hmje_async_handle_destroy(void *handle);
void *hmje_async_group_create(void);
void hmje_async_group_add(void *group, void *handle);
int32_t hmje_async_group_poll(void *group, void *parent_handle);
void hmje_async_group_finish(void *group, void *parent_handle);
void hmje_async_wait_for_wake(void);
/* Host sleep that ends early once any completion/cancel/failure lands after the
 * last hmje_async_unstructured_poll.  Returns 1 when woken early, 0 on timeout. */
int32_t hmje_async_sleep_until_wake_ms(uint64_t timeout_ms);
/* Vsync source hook (iOS CADisplayLink).  Bumps the frame generation reported
 * by hmje_async_sleep_until_wake_ms as bit 1.  Safe from any thread. */
void hmje_async_notify_frame(void);
uint64_t hmje_async_debug_blocking_wait_count(void);

#ifdef __cplusplus
}
#endif

#endif
