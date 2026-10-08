#ifndef HMJE_LEGACY_INPUT_H
#define HMJE_LEGACY_INPUT_H

/* Hosted UI가 SYS_INPUT_POLL에 내보내는 64-bit 레거시 입력 큐.
 *
 * DOWN/UP(CANCEL은 레거시 경로에서 UP으로 정규화)은 상태 전이이므로 절대
 * 표본처럼 버리면 안 된다. MOVE가 없는 순간에도 아직 소비하지 않은
 * DOWN/UP을 보존하도록 이 공통 큐는 다음 순서를 강제한다.
 *
 *   1. 연속 MOVE는 최신 좌표 하나로 합친다.
 *   2. 포화 시 MOVE/SCROLL 같은 transient 사건부터 회수한다.
 *   3. lifecycle만 남은 큐에 새 lifecycle이 오면 큐를 키워 FIFO를 보존한다.
 *
 * 메모리 부족 때 lifecycle을 조용히 잃고 영구 pressed 상태로 진행하는 것보다
 * 프로세스를 중단하는 편이 계약상 안전하다. 정상 경로는 256개 inline 슬롯만
 * 사용하고, 성장이 필요한 것은 소비자가 장시간 굶은 비정상 backlog뿐이다.
 */


#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define HMJE_LEGACY_INPUT_RING_INITIAL_CAP 256u
#define HMJE_LEGACY_INPUT_KIND_SHIFT 56u
#define HMJE_LEGACY_INPUT_KIND_POINTER_MOVE 1ull
#define HMJE_LEGACY_INPUT_KIND_POINTER_BUTTON 2ull

typedef struct hmje_legacy_input_ring {
    uint64_t inline_records[HMJE_LEGACY_INPUT_RING_INITIAL_CAP];
    uint64_t *grown_records;
    size_t capacity;
    size_t tail;
    size_t count;
    uint64_t dropped_transient;
    uint64_t growth_count;
} hmje_legacy_input_ring;

static inline size_t hmje_legacy_input_capacity(const hmje_legacy_input_ring *ring) {
    return ring->capacity == 0u ? HMJE_LEGACY_INPUT_RING_INITIAL_CAP : ring->capacity;
}

static inline uint64_t *hmje_legacy_input_records(hmje_legacy_input_ring *ring) {
    return ring->grown_records == NULL ? ring->inline_records : ring->grown_records;
}

static inline uint64_t hmje_legacy_input_kind(uint64_t value) {
    return value >> HMJE_LEGACY_INPUT_KIND_SHIFT;
}

static inline int hmje_legacy_input_is_lifecycle(uint64_t value) {
    uint64_t kind = hmje_legacy_input_kind(value);
    /* Keyboard transitions and committed text cannot be coalesced or dropped
     * either: a pasted string may exceed the inline queue in one message pump.
     * A composition update (kind 8) is characters followed by an END record;
     * dropping part of it would show a wrong preedit. */
    return kind == 0u || kind == HMJE_LEGACY_INPUT_KIND_POINTER_BUTTON || kind == 4u || kind == 6u || kind == 7u
        || kind == 8u;
}

static inline int hmje_legacy_input_grow(hmje_legacy_input_ring *ring) {
    size_t old_capacity = hmje_legacy_input_capacity(ring);
    if (old_capacity > SIZE_MAX / 2u || old_capacity * 2u > SIZE_MAX / sizeof(uint64_t)) {
        return 0;
    }
    size_t new_capacity = old_capacity * 2u;
    uint64_t *new_records = (uint64_t *)malloc(new_capacity * sizeof(uint64_t));
    if (new_records == NULL) return 0;

    uint64_t *old_records = hmje_legacy_input_records(ring);
    for (size_t i = 0u; i < ring->count; ++i) {
        new_records[i] = old_records[(ring->tail + i) % old_capacity];
    }
    if (ring->grown_records != NULL) free(ring->grown_records);
    ring->grown_records = new_records;
    ring->capacity = new_capacity;
    ring->tail = 0u;
    ring->growth_count += 1u;
    return 1;
}

/* 가장 오래된 transient 사건 하나를 없애되 나머지 FIFO 순서를 보존한다. */
static inline int hmje_legacy_input_reclaim_transient(hmje_legacy_input_ring *ring) {
    size_t capacity = hmje_legacy_input_capacity(ring);
    uint64_t *records = hmje_legacy_input_records(ring);
    size_t offset = 0u;
    while (offset < ring->count) {
        if (!hmje_legacy_input_is_lifecycle(records[(ring->tail + offset) % capacity])) {
            for (size_t cursor = offset; cursor + 1u < ring->count; ++cursor) {
                records[(ring->tail + cursor) % capacity] =
                    records[(ring->tail + cursor + 1u) % capacity];
            }
            ring->count -= 1u;
            ring->dropped_transient += 1u;
            return 1;
        }
        offset += 1u;
    }
    return 0;
}

static inline void hmje_legacy_input_ring_push(hmje_legacy_input_ring *ring, uint64_t value) {
    size_t capacity = hmje_legacy_input_capacity(ring);
    uint64_t *records = hmje_legacy_input_records(ring);
    uint64_t kind = hmje_legacy_input_kind(value);

    if (kind == HMJE_LEGACY_INPUT_KIND_POINTER_MOVE && ring->count > 0u) {
        size_t newest = (ring->tail + ring->count - 1u) % capacity;
        if (hmje_legacy_input_kind(records[newest]) == HMJE_LEGACY_INPUT_KIND_POINTER_MOVE) {
            records[newest] = value;
            ring->dropped_transient += 1u;
            return;
        }
    }

    if (ring->count == capacity) {
        if (!hmje_legacy_input_is_lifecycle(value)) {
            ring->dropped_transient += 1u;
            return;
        }
        if (!hmje_legacy_input_reclaim_transient(ring)) {
            if (!hmje_legacy_input_grow(ring)) abort();
        }
        capacity = hmje_legacy_input_capacity(ring);
        records = hmje_legacy_input_records(ring);
    }

    records[(ring->tail + ring->count) % capacity] = value;
    ring->count += 1u;
}

static inline int64_t hmje_legacy_input_ring_pop(hmje_legacy_input_ring *ring) {
    if (ring->count == 0u) return 0;
    size_t capacity = hmje_legacy_input_capacity(ring);
    uint64_t *records = hmje_legacy_input_records(ring);
    uint64_t value = records[ring->tail];
    ring->tail = (ring->tail + 1u) % capacity;
    ring->count -= 1u;
    if (ring->count == 0u) ring->tail = 0u;
    return (int64_t)value;
}

#endif
