#include <stdint.h>
void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
void hmje_ui_notification_request_async(uint64_t token) {
    hmje_async_complete_u64(token, 120, 0);
}
void hmje_ui_notification_click_wait_async(uint64_t token) {
    hmje_async_complete_u64(token, 120, 0);
}
void hmje_ui_notification_post_async(const uint8_t *title, uint64_t title_length,
    const uint8_t *body, uint64_t body_length, const uint8_t *identifier,
    uint64_t identifier_length, uint64_t token) {
    (void)title; (void)title_length; (void)body; (void)body_length;
    (void)identifier; (void)identifier_length;
    hmje_async_complete_u64(token, 120, 0);
}
int64_t hmje_ui_notification_click_next(uint8_t *output, uint64_t capacity, uint64_t *written) {
    (void)output; (void)capacity;
    if (written) *written = 0;
    return -120;
}
int64_t hmje_ui_notification_badge(int64_t number) { (void)number; return -120; }
