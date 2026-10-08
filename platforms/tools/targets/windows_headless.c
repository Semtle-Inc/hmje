#include <stdint.h>

typedef int64_t i64;

#define HMJE_WINDOWS_ENOSYS (-38LL)

i64 hmje_windows_framebuffer_info(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_framebuffer_blit(i64 buffer_address, i64 width, i64 height,
                                   i64 pitch, i64 x, i64 y) {
    (void)buffer_address;
    (void)width;
    (void)height;
    (void)pitch;
    (void)x;
    (void)y;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_framebuffer_present(i64 rects_address, i64 count) {
    (void)rects_address;
    (void)count;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_gpu_command(i64 kind, i64 request_address,
                             i64 request_length) {
    (void)kind;
    (void)request_address;
    (void)request_length;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_input_poll(void) {
    return 0;
}

i64 hmje_windows_input_poll2(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return 0;
}

i64 hmje_windows_system_theme(void) { return 0; }

i64 hmje_windows_text_input_control(i64 request) {
    (void)request;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_text_input_caret(i64 position, i64 size) {
    (void)position;
    (void)size;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_ui_window_config(i64 title_address, i64 title_length,
                                  i64 flags) {
    (void)title_address;
    (void)title_length;
    (void)flags;
    return HMJE_WINDOWS_ENOSYS;
}

i64 hmje_windows_ui_window(i64 operation, i64 argument1, i64 argument2) {
    (void)operation;
    (void)argument1;
    (void)argument2;
    return HMJE_WINDOWS_ENOSYS;
}
i64 hmje_windows_ui_drag_drop(i64 operation, i64 address, i64 capacity) {
    (void)operation; (void)address; (void)capacity;
    return -38;
}

/* 앱을 연 주소(SYS_APP_OPEN_URL 120). 창이 없는 명령행 실행 파일은 주소로 열리지 않는다. */
i64 hmje_windows_app_open_url(i64 operation, i64 address, i64 capacity) {
    (void)operation; (void)address; (void)capacity;
    return HMJE_WINDOWS_ENOSYS;
}
