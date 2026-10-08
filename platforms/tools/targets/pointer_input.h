#ifndef HMJE_POINTER_INPUT_H
#define HMJE_POINTER_INPUT_H

/* Hosted 포인터 표본 ABI와 bounded 큐.
 *
 * native backend는 OS 사건을 이 POD 레코드로 정규화해 게시만 한다. 영역
 * 라우팅, hit-test, gesture 인식은 HmjeUI가 맡는다. SYS_INPUT_POLL2는 이
 * 128-byte 레코드 하나를 호출자 버퍼로 복사한다.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HMJE_POINTER_RECORD_VERSION 1u
#define HMJE_POINTER_RECORD_SIZE 128u

#define HMJE_POINTER_KIND_POINTER 1u

#define HMJE_POINTER_PHASE_DOWN 1u
#define HMJE_POINTER_PHASE_MOVE 2u
#define HMJE_POINTER_PHASE_UP 3u
#define HMJE_POINTER_PHASE_CANCEL 4u
#define HMJE_POINTER_PHASE_HOVER 5u
#define HMJE_POINTER_PHASE_SCROLL 6u

#define HMJE_POINTER_TOOL_PEN 1u
#define HMJE_POINTER_TOOL_TOUCH 2u
#define HMJE_POINTER_TOOL_MOUSE 3u
#define HMJE_POINTER_TOOL_ERASER 4u
#define HMJE_POINTER_TOOL_UNKNOWN 5u

#define HMJE_POINTER_FLAG_PREDICTED (1ull << 0)
#define HMJE_POINTER_FLAG_COALESCED (1ull << 1)
#define HMJE_POINTER_CAP_PRESSURE (1ull << 8)
#define HMJE_POINTER_CAP_ALTITUDE (1ull << 9)
#define HMJE_POINTER_CAP_AZIMUTH (1ull << 10)
#define HMJE_POINTER_CAP_BUTTONS (1ull << 11)
#define HMJE_POINTER_CAP_SCROLL (1ull << 12)

typedef struct hmje_pointer_record {
    uint64_t version;
    uint64_t kind;
    uint64_t phase;
    uint64_t pointer_id;
    int64_t x;
    int64_t y;
    uint64_t time_us;
    uint64_t buttons;
    uint64_t changed_button;
    uint64_t pressure;
    uint64_t altitude;
    uint64_t azimuth;
    int64_t scroll_x;
    int64_t scroll_y;
    uint64_t tool;
    uint64_t flags;
} hmje_pointer_record;

typedef char hmje_pointer_record_size_must_be_128[
    sizeof(hmje_pointer_record) == HMJE_POINTER_RECORD_SIZE ? 1 : -1
];

#define HMJE_POINTER_RING_CAP 2048u
#define HMJE_POINTER_LIFECYCLE_RESERVE 64u

typedef struct hmje_pointer_ring {
    hmje_pointer_record records[HMJE_POINTER_RING_CAP];
    hmje_pointer_record *grown_records;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    volatile uint8_t lock;
    uint64_t dropped_moves;
    uint64_t dropped_lifecycle;
    uint64_t growth_count;
} hmje_pointer_ring;

static inline void hmje_pointer_lock(hmje_pointer_ring *ring) {
    while (__atomic_test_and_set(&ring->lock, __ATOMIC_ACQUIRE)) {
    }
}

static inline void hmje_pointer_unlock(hmje_pointer_ring *ring) {
    __atomic_clear(&ring->lock, __ATOMIC_RELEASE);
}

static inline uint32_t hmje_pointer_ring_capacity_locked(const hmje_pointer_ring *ring) {
    return ring->capacity == 0u ? HMJE_POINTER_RING_CAP : ring->capacity;
}

static inline hmje_pointer_record *hmje_pointer_ring_records_locked(hmje_pointer_ring *ring) {
    return ring->grown_records == NULL ? ring->records : ring->grown_records;
}

static inline uint32_t hmje_pointer_ring_count_locked(const hmje_pointer_ring *ring) {
    uint32_t capacity = hmje_pointer_ring_capacity_locked(ring);
    return (ring->head + capacity - ring->tail) % capacity;
}

static inline int hmje_pointer_is_motion(uint64_t phase) {
    return phase == HMJE_POINTER_PHASE_MOVE || phase == HMJE_POINTER_PHASE_HOVER;
}

static inline int hmje_pointer_is_transient(uint64_t phase) {
    return hmje_pointer_is_motion(phase) || phase == HMJE_POINTER_PHASE_SCROLL;
}

static inline int hmje_pointer_is_lifecycle(uint64_t phase) {
    return phase == HMJE_POINTER_PHASE_DOWN || phase == HMJE_POINTER_PHASE_UP ||
           phase == HMJE_POINTER_PHASE_CANCEL;
}

/* tail부터 첫 transient를 제거하고 나머지 FIFO 순서를 보존한다. */
static inline int hmje_pointer_reclaim_transient_locked(hmje_pointer_ring *ring) {
    uint32_t capacity = hmje_pointer_ring_capacity_locked(ring);
    hmje_pointer_record *records = hmje_pointer_ring_records_locked(ring);
    uint32_t count = hmje_pointer_ring_count_locked(ring);
    for (uint32_t offset = 0u; offset < count; ++offset) {
        uint32_t index = (ring->tail + offset) % capacity;
        if (hmje_pointer_is_transient(records[index].phase)) {
            for (uint32_t cursor = offset; cursor + 1u < count; ++cursor) {
                records[(ring->tail + cursor) % capacity] =
                    records[(ring->tail + cursor + 1u) % capacity];
            }
            ring->head = (ring->head + capacity - 1u) % capacity;
            ring->dropped_moves += 1u;
            return 1;
        }
    }
    return 0;
}

/* lifecycle만으로 큐가 찬 경우 DOWN/UP/CANCEL을 버리지 않고 저장소를 키운다. */
static inline int hmje_pointer_ring_grow_locked(hmje_pointer_ring *ring) {
    uint32_t old_capacity = hmje_pointer_ring_capacity_locked(ring);
    if (old_capacity > UINT32_MAX / 2u) return 0;
    uint32_t new_capacity = old_capacity * 2u;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(hmje_pointer_record)) return 0;
    hmje_pointer_record *new_records =
        (hmje_pointer_record *)malloc((size_t)new_capacity * sizeof(hmje_pointer_record));
    if (new_records == NULL) return 0;

    hmje_pointer_record *old_records = hmje_pointer_ring_records_locked(ring);
    uint32_t count = hmje_pointer_ring_count_locked(ring);
    for (uint32_t i = 0u; i < count; ++i) {
        new_records[i] = old_records[(ring->tail + i) % old_capacity];
    }
    if (ring->grown_records != NULL) free(ring->grown_records);
    ring->grown_records = new_records;
    ring->capacity = new_capacity;
    ring->tail = 0u;
    ring->head = count;
    ring->growth_count += 1u;
    return 1;
}

static inline void hmje_pointer_ring_push(hmje_pointer_ring *ring, hmje_pointer_record record) {
    record.version = HMJE_POINTER_RECORD_VERSION;
    record.kind = HMJE_POINTER_KIND_POINTER;
    if (record.pressure > 65535u) record.pressure = 65535u;
    if (record.altitude > 65535u) record.altitude = 65535u;
    if (record.azimuth > 65535u) record.azimuth = 65535u;

    hmje_pointer_lock(ring);
    uint32_t count = hmje_pointer_ring_count_locked(ring);
    uint32_t capacity = hmje_pointer_ring_capacity_locked(ring);
    hmje_pointer_record *records = hmje_pointer_ring_records_locked(ring);
    if (hmje_pointer_is_transient(record.phase) &&
        count + HMJE_POINTER_LIFECYCLE_RESERVE >= capacity - 1u) {
        if (hmje_pointer_is_motion(record.phase) && count > 0u) {
            uint32_t newest = (ring->head + capacity - 1u) % capacity;
            hmje_pointer_record *last = &records[newest];
            if (hmje_pointer_is_motion(last->phase) && last->pointer_id == record.pointer_id &&
                ((last->flags ^ record.flags) & HMJE_POINTER_FLAG_PREDICTED) == 0u) {
                *last = record;
                ring->dropped_moves += 1u;
                hmje_pointer_unlock(ring);
                return;
            }
        }
        if (!hmje_pointer_reclaim_transient_locked(ring)) {
            ring->dropped_moves += 1u;
            hmje_pointer_unlock(ring);
            return;
        }
    }

    capacity = hmje_pointer_ring_capacity_locked(ring);
    records = hmje_pointer_ring_records_locked(ring);
    uint32_t next = (ring->head + 1u) % capacity;
    if (next == ring->tail) {
        if (hmje_pointer_is_lifecycle(record.phase)) {
            if (!hmje_pointer_reclaim_transient_locked(ring)) {
                /* lifecycle 손실은 캡처/pressed 상태를 영구 오염시킨다. 큐를
                 * 키울 수조차 없는 OOM에서는 조용히 계속하지 않는다. */
                if (!hmje_pointer_ring_grow_locked(ring)) abort();
            }
        } else {
            if (!hmje_pointer_reclaim_transient_locked(ring)) {
                ring->dropped_moves += 1u;
                hmje_pointer_unlock(ring);
                return;
            }
        }
        capacity = hmje_pointer_ring_capacity_locked(ring);
        records = hmje_pointer_ring_records_locked(ring);
        next = (ring->head + 1u) % capacity;
    }
    records[ring->head] = record;
    ring->head = next;
    hmje_pointer_unlock(ring);
}

static inline int64_t hmje_pointer_ring_pop(
    hmje_pointer_ring *ring, int64_t output_address, int64_t output_length
) {
    if (output_address == 0 || output_length < (int64_t)HMJE_POINTER_RECORD_SIZE) {
        return -22; /* ERR_INVALID_ARGUMENT / EINVAL */
    }
    hmje_pointer_lock(ring);
    if (ring->head == ring->tail) {
        hmje_pointer_unlock(ring);
        return 0;
    }
    uint32_t capacity = hmje_pointer_ring_capacity_locked(ring);
    hmje_pointer_record *records = hmje_pointer_ring_records_locked(ring);
    hmje_pointer_record record = records[ring->tail];
    ring->tail = (ring->tail + 1u) % capacity;
    hmje_pointer_unlock(ring);
    memcpy((void *)(uintptr_t)(uint64_t)output_address, &record, sizeof(record));
    return (int64_t)sizeof(record);
}

#endif
