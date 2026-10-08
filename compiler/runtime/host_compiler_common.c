/* Portable compiler host helpers shared by the POSIX host (host_support.c)
 * and the one-source Windows bundle (host_support_windows.c): backend worker
 * fork hooks (fork-less hosts report -1 and the backend lowers serially),
 * HMIR string renumbering, the dependency-check memo, hashing, byte search,
 * name mangling and dependency recording. Both hosts export the same
 * hmje_compiler_* symbols. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <fcntl.h>
#include <time.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define HMJE_MEMO_LOCK_DEFINE static SRWLOCK hmje_memo_lock = SRWLOCK_INIT;
#define HMJE_MEMO_LOCK() AcquireSRWLockExclusive(&hmje_memo_lock)
#define HMJE_MEMO_UNLOCK() ReleaseSRWLockExclusive(&hmje_memo_lock)
#else
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define HMJE_MEMO_LOCK_DEFINE static pthread_mutex_t hmje_memo_lock = PTHREAD_MUTEX_INITIALIZER;
#define HMJE_MEMO_LOCK() pthread_mutex_lock(&hmje_memo_lock)
#define HMJE_MEMO_UNLOCK() pthread_mutex_unlock(&hmje_memo_lock)
#endif


#include "host_scratch_os.c"

#if defined(_WIN32)
/* No hard-link publication on Windows yet: callers fall back to the atomic
 * copy (포직스파일원자복사) whenever this returns non-zero. */
int64_t hmje_compiler_link_published(uint64_t source_address, uint64_t source_length,
                                     uint64_t target_address, uint64_t target_length) {
    (void)source_address; (void)source_length; (void)target_address; (void)target_length;
    return -ENOSYS;
}
#endif

/* Fork for the backend's function-lowering workers. The backend is single
 * threaded when it forks, so the child owns a private copy-on-write heap. */
int64_t hmje_compiler_fork(void) {
#if defined(_WIN32)
    return -1;
#else
    fflush(NULL);
    pid_t child = fork();
    return child < 0 ? -1 : (int64_t)child;
#endif
}

/* Wait for a forked worker: its exit code, or -1 if it did not exit normally. */
int64_t hmje_compiler_wait_child(int64_t child) {
#if defined(_WIN32)
    (void)child;
    return -1;
#else
    int status = 0;
    pid_t done;
    do { done = waitpid((pid_t)child, &status, 0); } while (done < 0 && errno == EINTR);
    if (done != (pid_t)child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

/* Stop a forked worker that will not be used (a later fork failed). */
int64_t hmje_compiler_kill_child(int64_t child) {
#if defined(_WIN32)
    (void)child;
    return -1;
#else
    return kill((pid_t)child, SIGKILL) == 0 ? 0 : -(int64_t)errno;
#endif
}

/* A forked worker leaves without atexit handlers or stdio flushing that belong
 * to the parent. */
int64_t hmje_compiler_exit_now(int64_t code) {
    _exit((int)code);
    return 0;
}

/* Rewrite every `@str.<n>` outside "..." as `@str.<n + offset>`. Returns the
 * output length; writes only when it fits in capacity (call with capacity 0
 * to size the buffer). */
int64_t hmje_compiler_renumber_string_globals(uint64_t input_address, uint64_t input_length,
                                              uint64_t offset,
                                              uint64_t output_address, uint64_t capacity) {
    const uint8_t *in = (const uint8_t *)(uintptr_t)input_address;
    uint8_t *out = (uint8_t *)(uintptr_t)output_address;
    if (!in && input_length) return -1;
    uint64_t used = 0, position = 0;
#define HMJE_RENUMBER_PUT(byte) do { if (out && used < capacity) out[used] = (byte); used++; } while (0)
    while (position < input_length) {
        uint8_t byte = in[position];
        if (byte == 34) {
            HMJE_RENUMBER_PUT(byte);
            position += 1;
            while (position < input_length) {
                uint8_t quoted = in[position++];
                HMJE_RENUMBER_PUT(quoted);
                if (quoted == 92 && position < input_length) { HMJE_RENUMBER_PUT(in[position]); position++; }
                else if (quoted == 34) break;
            }
            continue;
        }
        if (byte == '@' && position + 5 < input_length && memcmp(in + position, "@str.", 5) == 0 &&
            in[position + 5] >= '0' && in[position + 5] <= '9') {
            uint64_t end = position + 5, number = 0;
            while (end < input_length && in[end] >= '0' && in[end] <= '9') {
                number = number * 10 + (uint64_t)(in[end] - '0');
                end += 1;
            }
            int identifier = end < input_length &&
                ((in[end] >= 'a' && in[end] <= 'z') || (in[end] >= 'A' && in[end] <= 'Z') ||
                 in[end] == '_' || in[end] == '.' || in[end] == '$' || in[end] == '-');
            if (!identifier) {
                char digits[24];
                int count = snprintf(digits, sizeof digits, "%llu", (unsigned long long)(number + offset));
                for (unsigned i = 0; i < 5; ++i) HMJE_RENUMBER_PUT(in[position + i]);
                for (int i = 0; i < count; ++i) HMJE_RENUMBER_PUT((uint8_t)digits[i]);
                position = end;
                continue;
            }
        }
        HMJE_RENUMBER_PUT(byte);
        position += 1;
    }
#undef HMJE_RENUMBER_PUT
    return (int64_t)used;
}

/* Exact-key memo for the function HMIR cache's dependency checks: (name bytes,
 * version) -> value. The frontend validates the same document symbols for
 * thousands of cached functions; the version is the symbol-list length, which
 * only grows between clears, and the frontend clears the memo whenever it
 * refreshes the document state. Keys are copied; a mutex guards the table. */
struct hmje_memo_entry { uint8_t *key; uint64_t length, version, value, hash; };
static struct hmje_memo_entry *hmje_memo_slots;
static uint64_t hmje_memo_capacity, hmje_memo_count;
HMJE_MEMO_LOCK_DEFINE

static uint64_t hmje_memo_hash(const uint8_t *key, uint64_t length, uint64_t version) {
    uint64_t hash = UINT64_C(1469598103934665603) ^ version;
    for (uint64_t i = 0; i < length; ++i) { hash ^= key[i]; hash *= UINT64_C(1099511628211); }
    return hash ? hash : 1;
}

int64_t hmje_compiler_memo_lookup(uint64_t key_address, uint64_t key_length, uint64_t version,
                                  uint64_t value_address) {
    const uint8_t *key = (const uint8_t *)(uintptr_t)key_address;
    if ((!key && key_length) || !value_address) return 0;
    uint64_t hash = hmje_memo_hash(key, key_length, version);
    int64_t found = 0;
    HMJE_MEMO_LOCK();
    if (hmje_memo_capacity) {
        for (uint64_t slot = hash & (hmje_memo_capacity - 1);; slot = (slot + 1) & (hmje_memo_capacity - 1)) {
            struct hmje_memo_entry *entry = &hmje_memo_slots[slot];
            if (!entry->hash) break;
            if (entry->hash == hash && entry->version == version && entry->length == key_length &&
                memcmp(entry->key, key, (size_t)key_length) == 0) {
                *(uint64_t *)(uintptr_t)value_address = entry->value;
                found = 1;
                break;
            }
        }
    }
    HMJE_MEMO_UNLOCK();
    return found;
}

int64_t hmje_compiler_memo_clear(void) {
    HMJE_MEMO_LOCK();
    for (uint64_t i = 0; i < hmje_memo_capacity; ++i) free(hmje_memo_slots[i].key);
    free(hmje_memo_slots);
    hmje_memo_slots = NULL;
    hmje_memo_capacity = hmje_memo_count = 0;
    HMJE_MEMO_UNLOCK();
    return 0;
}

int64_t hmje_compiler_memo_store(uint64_t key_address, uint64_t key_length, uint64_t version,
                                 uint64_t value) {
    const uint8_t *key = (const uint8_t *)(uintptr_t)key_address;
    if (!key && key_length) return -EINVAL;
    uint64_t hash = hmje_memo_hash(key, key_length, version);
    int64_t status = 0;
    HMJE_MEMO_LOCK();
    if ((hmje_memo_count + 1) * 2 > hmje_memo_capacity) {
        uint64_t capacity = hmje_memo_capacity ? hmje_memo_capacity * 2 : 4096;
        struct hmje_memo_entry *slots = calloc((size_t)capacity, sizeof *slots);
        if (!slots) { HMJE_MEMO_UNLOCK(); return -ENOMEM; }
        for (uint64_t i = 0; i < hmje_memo_capacity; ++i) {
            struct hmje_memo_entry entry = hmje_memo_slots[i];
            if (!entry.hash) continue;
            uint64_t slot = entry.hash & (capacity - 1);
            while (slots[slot].hash) slot = (slot + 1) & (capacity - 1);
            slots[slot] = entry;
        }
        free(hmje_memo_slots);
        hmje_memo_slots = slots;
        hmje_memo_capacity = capacity;
    }
    uint64_t slot = hash & (hmje_memo_capacity - 1);
    for (;; slot = (slot + 1) & (hmje_memo_capacity - 1)) {
        struct hmje_memo_entry *entry = &hmje_memo_slots[slot];
        if (!entry->hash) {
            uint8_t *copy = malloc(key_length ? (size_t)key_length : 1);
            if (!copy) { status = -ENOMEM; break; }
            if (key_length) memcpy(copy, key, (size_t)key_length);
            entry->key = copy; entry->length = key_length; entry->version = version;
            entry->value = value; entry->hash = hash;
            hmje_memo_count += 1;
            break;
        }
        if (entry->hash == hash && entry->version == version && entry->length == key_length &&
            memcmp(entry->key, key, (size_t)key_length) == 0) {
            entry->value = value;
            break;
        }
    }
    HMJE_MEMO_UNLOCK();
    return status;
}

/* FNV-1a 64 continuation over caller-owned bytes: exactly the fold of
 * 해시에바이트섞기 byte by byte, for cache keys over whole files and modules. */
uint64_t hmje_compiler_fnv1a_bytes(uint64_t seed, uint64_t address, uint64_t length) {
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;
    uint64_t hash = seed;
    if (!bytes) return hash;
    for (uint64_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* memmem for compiler hot paths: first index of needle at or after start, or -1.
 * An empty needle matches at start (like 문자열찾기 at 0). */
int64_t hmje_compiler_find_bytes(uint64_t haystack_address, uint64_t haystack_length,
                                 uint64_t needle_address, uint64_t needle_length,
                                 uint64_t start) {
    const uint8_t *hay = (const uint8_t *)(uintptr_t)haystack_address;
    const uint8_t *needle = (const uint8_t *)(uintptr_t)needle_address;
    if (start > haystack_length) return -1;
    if (needle_length == 0) return (int64_t)start;
    if (!hay || !needle || needle_length > haystack_length - start) return -1;
    const uint8_t *cursor = hay + start;
    const uint8_t *last = hay + haystack_length - needle_length;
    while (cursor <= last) {
        const uint8_t *hit = memchr(cursor, needle[0], (size_t)(last - cursor) + 1);
        if (!hit) return -1;
        if (memcmp(hit, needle, (size_t)needle_length) == 0) return (int64_t)(hit - hay);
        cursor = hit + 1;
    }
    return -1;
}

/* LLVM분할인용밖포함: does needle occur outside "..." strings (backslash escapes
 * the next byte inside a string)? Same scan as the HMJE version. */
int64_t hmje_compiler_contains_outside_quotes(uint64_t text_address, uint64_t text_length,
                                              uint64_t needle_address, uint64_t needle_length) {
    const uint8_t *text = (const uint8_t *)(uintptr_t)text_address;
    const uint8_t *needle = (const uint8_t *)(uintptr_t)needle_address;
    if (!needle_length || (!text && text_length) || !needle) return 0;
    uint64_t position = 0;
    while (position < text_length) {
        uint8_t byte = text[position];
        if (byte == 34) {
            position += 1;
            while (position < text_length) {
                uint8_t quoted = text[position];
                position += 1;
                if (quoted == 92 && position < text_length) position += 1;
                else if (quoted == 34) break;
            }
            continue;
        }
        if (position + needle_length <= text_length && byte == needle[0] &&
            memcmp(text + position, needle, (size_t)needle_length) == 0) return 1;
        position += 1;
    }
    return 0;
}

/* Same bytes as 맹글이름 (LLVM/이름맹글링.hmje, lower_llvm.mangle_name): ASCII
 * letters, digits and '_' stay, every other code point becomes u<lowercase hex>,
 * pieces are joined with '_'. Returns the output length or -1 if it would not
 * fit. Bytes past the end read as 0, like a truncated sequence in HMJE. */
int64_t hmje_compiler_mangle_name(uint64_t input_address, uint64_t input_length,
                                  uint64_t output_address, uint64_t capacity) {
    const uint8_t *in = (const uint8_t *)(uintptr_t)input_address;
    uint8_t *out = (uint8_t *)(uintptr_t)output_address;
    if ((!in && input_length) || !out) return -1;
    if (input_length == 0) {
        if (capacity < 1) return -1;
        out[0] = '_';
        return 1;
    }
    uint64_t used = 0, position = 0;
    int first_piece = 1;
    while (position < input_length) {
#define HMJE_MANGLE_BYTE(i) ((position + (i)) < input_length ? (uint64_t)in[position + (i)] : 0)
        uint64_t lead = in[position], code, width;
        if (lead < 128) { code = lead; width = 1; }
        else if (lead < 224) { code = ((lead & 31) << 6) | (HMJE_MANGLE_BYTE(1) & 63); width = 2; }
        else if (lead < 240) {
            code = ((lead & 15) << 12) | ((HMJE_MANGLE_BYTE(1) & 63) << 6) | (HMJE_MANGLE_BYTE(2) & 63);
            width = 3;
        } else {
            code = ((lead & 7) << 18) | ((HMJE_MANGLE_BYTE(1) & 63) << 12) |
                   ((HMJE_MANGLE_BYTE(2) & 63) << 6) | (HMJE_MANGLE_BYTE(3) & 63);
            width = 4;
        }
#undef HMJE_MANGLE_BYTE
        if (!first_piece) {
            if (used + 1 > capacity) return -1;
            out[used++] = '_';
        }
        first_piece = 0;
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') ||
            (code >= 'a' && code <= 'z') || code == '_') {
            if (used + 1 > capacity) return -1;
            out[used++] = (uint8_t)code;
        } else {
            char digits[24];
            unsigned count = 0;
            uint64_t rest = code;
            do { digits[count++] = "0123456789abcdef"[rest & 15]; rest >>= 4; } while (rest);
            if (used + 1 + count > capacity) return -1;
            out[used++] = 'u';
            while (count) out[used++] = (uint8_t)digits[--count];
        }
        position += width;
    }
    return (int64_t)used;
}


/* Function HMIR dependency recorder. Each thread keeps a stack of frames, one
 * per function whose HMIR is being generated. Environment lookups made while a
 * frame is open append "<kind><name>\n" to the innermost frame. Ending a frame
 * returns its bytes and appends them to the enclosing frame too, because the
 * outer function's output then contains whatever the inner one produced.
 * Lookups outside any frame (document-level passes) are not recorded. */
typedef struct {
    char *data;
    uint64_t length;
    uint64_t capacity;
    /* Symbols below this index were inherited from the document (globals). */
    uint64_t base;
} hmje_compiler_dep_frame;

static _Thread_local hmje_compiler_dep_frame *hmje_compiler_dep_frames;
static _Thread_local uint64_t hmje_compiler_dep_depth;
static _Thread_local uint64_t hmje_compiler_dep_frame_capacity;
/* Frames whose begin failed to allocate. They are still matched by dep_end,
 * which then reports -1 and poisons the enclosing frame. */
static _Thread_local uint64_t hmje_compiler_dep_lost;

static int hmje_compiler_dep_append(hmje_compiler_dep_frame *frame,
                                    const char *bytes, uint64_t length) {
    if (frame->length + length > frame->capacity) {
        uint64_t capacity = frame->capacity ? frame->capacity : 256;
        while (capacity < frame->length + length) capacity *= 2;
        char *grown = realloc(frame->data, capacity);
        if (!grown) return -1;
        frame->data = grown;
        frame->capacity = capacity;
    }
    memcpy(frame->data + frame->length, bytes, length);
    frame->length += length;
    return 0;
}

int64_t hmje_compiler_dep_begin(uint64_t base) {
    if (hmje_compiler_dep_lost) {
        hmje_compiler_dep_lost += 1;
        return -1;
    }
    if (hmje_compiler_dep_depth == hmje_compiler_dep_frame_capacity) {
        uint64_t capacity = hmje_compiler_dep_frame_capacity ? hmje_compiler_dep_frame_capacity * 2 : 8;
        hmje_compiler_dep_frame *grown = realloc(hmje_compiler_dep_frames,
                                                 capacity * sizeof(hmje_compiler_dep_frame));
        if (!grown) {
            hmje_compiler_dep_lost = 1;
            return -1;
        }
        memset(grown + hmje_compiler_dep_frame_capacity, 0,
               (capacity - hmje_compiler_dep_frame_capacity) * sizeof(hmje_compiler_dep_frame));
        hmje_compiler_dep_frames = grown;
        hmje_compiler_dep_frame_capacity = capacity;
    }
    hmje_compiler_dep_frames[hmje_compiler_dep_depth].length = 0;
    hmje_compiler_dep_frames[hmje_compiler_dep_depth].base = base;
    hmje_compiler_dep_depth += 1;
    return 0;
}

/* Global-symbol boundary of the innermost frame, or UINT64_MAX when no frame
 * is open (lookups are then not recorded). */
uint64_t hmje_compiler_dep_base(void) {
    if (hmje_compiler_dep_lost || hmje_compiler_dep_depth == 0) return UINT64_MAX;
    return hmje_compiler_dep_frames[hmje_compiler_dep_depth - 1].base;
}

/* A failed append poisons the frame (length UINT64_MAX): the caller must not
 * publish a cache entry whose dependency list is incomplete. */
int64_t hmje_compiler_dep_record(uint64_t kind, uint64_t name_address, uint64_t name_length) {
    if (hmje_compiler_dep_lost) return -1;
    if (hmje_compiler_dep_depth == 0) return 0;
    hmje_compiler_dep_frame *frame = &hmje_compiler_dep_frames[hmje_compiler_dep_depth - 1];
    if (frame->length == UINT64_MAX) return -1;
    char head = (char)kind;
    char tail = '\n';
    if (hmje_compiler_dep_append(frame, &head, 1) != 0 ||
        (name_length && hmje_compiler_dep_append(frame, (const char *)(uintptr_t)name_address, name_length) != 0) ||
        hmje_compiler_dep_append(frame, &tail, 1) != 0) {
        frame->length = UINT64_MAX;
        return -1;
    }
    return 0;
}

/* Length of the innermost frame, or -1 if it is poisoned or none is open.
 * Callers size a buffer with this, then pop the frame with dep_end. */
int64_t hmje_compiler_dep_length(void) {
    if (hmje_compiler_dep_lost || hmje_compiler_dep_depth == 0) return -1;
    hmje_compiler_dep_frame *frame = &hmje_compiler_dep_frames[hmje_compiler_dep_depth - 1];
    if (frame->length == UINT64_MAX) return -1;
    return (int64_t)frame->length;
}

/* Pops the innermost frame, copying at most `capacity` bytes to `output`.
 * Returns the frame's length, or -1 if it was poisoned or none is open. */
int64_t hmje_compiler_dep_end(uint64_t output, uint64_t capacity) {
    if (hmje_compiler_dep_lost) {
        hmje_compiler_dep_lost -= 1;
        if (hmje_compiler_dep_depth > 0)
            hmje_compiler_dep_frames[hmje_compiler_dep_depth - 1].length = UINT64_MAX;
        return -1;
    }
    if (hmje_compiler_dep_depth == 0) return -1;
    hmje_compiler_dep_depth -= 1;
    hmje_compiler_dep_frame *frame = &hmje_compiler_dep_frames[hmje_compiler_dep_depth];
    int poisoned = frame->length == UINT64_MAX;
    if (hmje_compiler_dep_depth > 0) {
        hmje_compiler_dep_frame *outer = &hmje_compiler_dep_frames[hmje_compiler_dep_depth - 1];
        if (poisoned) {
            outer->length = UINT64_MAX;
        } else if (outer->length != UINT64_MAX &&
                   hmje_compiler_dep_append(outer, frame->data, frame->length) != 0) {
            outer->length = UINT64_MAX;
        }
    }
    if (poisoned) return -1;
    uint64_t copy = frame->length < capacity ? frame->length : capacity;
    if (copy && output) memcpy((void *)(uintptr_t)output, frame->data, copy);
    return (int64_t)frame->length;
}
