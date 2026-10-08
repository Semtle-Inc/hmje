#include <stdint.h>

typedef int64_t i64;

/*
 * Cocoa 창을 쓸 수 없는 hosted POSIX 타깃(Linux 등)의 정식 headless 경계다.
 * 파일·소켓·시간 syscall은 posix_host_shim.c가 계속 제공하고, 화면/GPU만
 * 명백한 ENOSYS로 닫는다. 입력 poll은 이벤트가 없다는 뜻의 0을 반환한다.
 */
#define HMJE_POSIX_ENOSYS (-38LL)

i64 hmje_posix_framebuffer_info(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return HMJE_POSIX_ENOSYS;
}

i64 hmje_posix_framebuffer_blit(
    i64 buffer_address,
    i64 width,
    i64 height,
    i64 pitch,
    i64 x,
    i64 y
) {
    (void)buffer_address;
    (void)width;
    (void)height;
    (void)pitch;
    (void)x;
    (void)y;
    return HMJE_POSIX_ENOSYS;
}

i64 hmje_posix_framebuffer_present(i64 rects_address, i64 count) {
    (void)rects_address;
    (void)count;
    return HMJE_POSIX_ENOSYS;
}

i64 hmje_posix_input_poll(void) {
    return 0;
}

i64 hmje_posix_input_poll2(i64 out_address, i64 out_length) {
    (void)out_address;
    (void)out_length;
    return 0;
}

i64 hmje_posix_gpu_command(i64 kind, i64 request_address, i64 request_length) {
    if (kind == 0 && request_address != 0 && request_length == 32) {
        uint64_t *output=(uint64_t *)(uintptr_t)(uint64_t)request_address;
        output[0]=2; output[1]=4; output[2]=0; output[3]=1;
        return 32;
    }
    return HMJE_POSIX_ENOSYS;
}
