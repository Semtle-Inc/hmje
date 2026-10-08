#include <errno.h>
#include <stdint.h>
void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
int64_t hmje_ui_watch_start(const uint8_t *root, uint64_t root_length,
    const uint8_t *excludes, uint64_t exclude_length, uint64_t delay_ms) {
    (void)root; (void)root_length; (void)excludes; (void)exclude_length; (void)delay_ms;
    return -120;
}
int64_t hmje_ui_watch_stop(int64_t handle) { (void)handle; return -120; }
int64_t hmje_ui_watch_next(int64_t handle, uint8_t *output, uint64_t capacity, uint64_t *written) {
    (void)handle; (void)output; (void)capacity;
    if (written) *written = 0;
    return -120;
}
void hmje_ui_watch_wait_async(int64_t handle, uint64_t token) {
    (void)handle;
    hmje_async_complete_u64(token, ENOSYS, 0);
}
