/* posix_host_fiber.h — 창 흐름(fiber)용 최소 문맥 교환(macOS arm64·x86_64).
 * hmje_fiber_switch(&저장_sp, 불러올_sp): 호출자 보존 레지스터를 지금 스택에
 * 쌓고 sp를 저장한 뒤, 다른 스택으로 옮겨 그쪽 레지스터를 되살리고 돌아간다.
 * 새 흐름 스택은 hmje_fiber_prepare가 "처음 돌아가면 trampoline이 불리는"
 * 모양으로 만든다. trampoline은 저장된 첫 보존 레지스터(arm64 x19, x86_64 rbx)를
 * 인자로 hmje_fiber_entry_c를 부르고 돌아오지 않는다. */
#ifndef HMJE_POSIX_HOST_FIBER_H
#define HMJE_POSIX_HOST_FIBER_H
#include <stdint.h>
#include <string.h>

void hmje_fiber_switch(void **save_sp, void *load_sp);
void hmje_fiber_entry_c(void *argument);

#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _hmje_fiber_switch\n"
    "_hmje_fiber_switch:\n"
    "    sub sp, sp, #160\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #160\n"
    "    ret\n"
    ".p2align 2\n"
    "_hmje_fiber_trampoline:\n"
    "    mov x0, x19\n"
    "    bl _hmje_fiber_entry_c\n"
    "    brk #0\n"
);
void hmje_fiber_trampoline(void) __asm__("_hmje_fiber_trampoline");
/* 스택 꼭대기(높은 주소)에서 교환 프레임 하나를 만든다. */
static inline void *hmje_fiber_prepare(void *stack_top, void *argument) {
    uintptr_t top = ((uintptr_t)stack_top) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - 160 - 16);
    memset(frame, 0, 160 + 16);
    frame[0] = (uint64_t)(uintptr_t)argument;              /* x19 */
    frame[10] = 0;                                         /* x29 = 끝 프레임 */
    frame[11] = (uint64_t)(uintptr_t)&hmje_fiber_trampoline; /* x30 */
    return frame;
}
#elif defined(__x86_64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _hmje_fiber_switch\n"
    "_hmje_fiber_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    stmxcsr (%rsp)\n"
    "    fnstcw 4(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    fldcw 4(%rsp)\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".p2align 4\n"
    "_hmje_fiber_trampoline:\n"
    "    movq %rbx, %rdi\n"
    "    andq $-16, %rsp\n"
    "    callq _hmje_fiber_entry_c\n"
    "    ud2\n"
);
void hmje_fiber_trampoline(void) __asm__("_hmje_fiber_trampoline");
static inline void *hmje_fiber_prepare(void *stack_top, void *argument) {
    uintptr_t top = ((uintptr_t)stack_top) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - 8 * 10);
    memset(frame, 0, 8 * 10);
    uint32_t mxcsr = 0x1f80; uint16_t fpucw = 0x037f;
    memcpy(&frame[0], &mxcsr, 4);
    memcpy((char *)&frame[0] + 4, &fpucw, 2);
    /* frame[1..6] = r15 r14 r13 r12 rbx rbp */
    frame[5] = (uint64_t)(uintptr_t)argument;               /* rbx */
    frame[7] = (uint64_t)(uintptr_t)&hmje_fiber_trampoline; /* ret */
    return frame;
}
#else
#error "hmje fiber switch: unsupported architecture"
#endif
#endif
