#ifndef HMJE_GUARDED_STACK_H
#define HMJE_GUARDED_STACK_H

/* OS boundary: mmap flags and page alignment differ across POSIX hosts. */
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct HmjeGuardedStack {
    void *mapping;
    size_t mapping_size;
    void *usable;
    size_t usable_size;
} HmjeGuardedStack;

static inline int hmje_guarded_stack_allocate(HmjeGuardedStack *stack,
                                             size_t bytes, size_t guard_bytes) {
    long page_query = sysconf(_SC_PAGESIZE);
    if (page_query <= 0) return EINVAL;
    size_t page = (size_t)page_query;
    if (guard_bytes < page) guard_bytes = page;
    if (bytes == 0 || bytes > SIZE_MAX - (page - 1) ||
        guard_bytes > SIZE_MAX - (page - 1)) return EOVERFLOW;
    size_t usable = ((bytes + page - 1) / page) * page;
    size_t guard = ((guard_bytes + page - 1) / page) * page;
    if (guard > (SIZE_MAX - usable) / 2) return EOVERFLOW;
    size_t total = usable + 2 * guard;
#ifdef MAP_ANONYMOUS
    int anonymous_flag = MAP_ANONYMOUS;
#else
    int anonymous_flag = MAP_ANON;
#endif
    void *base = mmap(NULL, total, PROT_NONE,
                      MAP_PRIVATE | anonymous_flag, -1, 0);
    if (base == MAP_FAILED) return errno;
    void *writable = (char *)base + guard;
    if (mprotect(writable, usable, PROT_READ | PROT_WRITE) != 0) {
        int error = errno;
        munmap(base, total);
        return error;
    }
    *stack = (HmjeGuardedStack){base, total, writable, usable};
    return 0;
}

static inline void hmje_guarded_stack_release(HmjeGuardedStack *stack) {
    if (stack->mapping != NULL) munmap(stack->mapping, stack->mapping_size);
    *stack = (HmjeGuardedStack){0};
}
#endif
