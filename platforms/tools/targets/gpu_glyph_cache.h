#ifndef HMJE_GPU_GLYPH_CACHE_H
#define HMJE_GPU_GLYPH_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Renderer-thread-owned, content-addressed immutable glyphs. Source addresses
 * and row padding are deliberately not identities: the font atlas can move or
 * reuse an address. Hash hits are always checked against an owned packed copy.
 * Eight ways prevent the two-glyph ping-pong of the old direct-mapped cache. */
#define HMJE_GLYPH_CACHE_SLOTS 1024u
#define HMJE_GLYPH_CACHE_WAYS 8u
#define HMJE_GLYPH_CACHE_BYTES (8u * 1024u * 1024u)
typedef struct {
    uint64_t hash, width, height, used;
    unsigned char *bytes;
} hmje_glyph_cache_entry;
typedef struct {
    hmje_glyph_cache_entry entries[HMJE_GLYPH_CACHE_SLOTS];
    uint64_t clock;
    size_t bytes;
} hmje_glyph_cache;
typedef void (*hmje_glyph_cache_release)(unsigned slot);

static inline void hmje_glyph_cache_drop(hmje_glyph_cache *cache, unsigned slot,
                                  hmje_glyph_cache_release release) {
    hmje_glyph_cache_entry *entry = &cache->entries[slot];
    if (entry->bytes == NULL) return;
    cache->bytes -= (size_t)(entry->width * entry->height);
    free(entry->bytes);
    memset(entry, 0, sizeof(*entry));
    if (release) release(slot);
}

static inline void hmje_glyph_cache_clear(hmje_glyph_cache *cache,
                                   hmje_glyph_cache_release release) {
    for (unsigned slot = 0; slot < HMJE_GLYPH_CACHE_SLOTS; ++slot)
        hmje_glyph_cache_drop(cache, slot, release);
    cache->clock = 0;
}

/* Returns a resident slot, or -1 for invalid/oversize/out-of-memory input.
 * Oversize glyphs may still be uploaded by the caller without caching.
 * A newly inserted slot has no native texture until the caller installs one;
 * on upload failure the caller must drop that slot. */
static inline int hmje_glyph_cache_get(hmje_glyph_cache *cache,
                                const unsigned char *source,
                                uint64_t width, uint64_t height, uint64_t pitch,
                                hmje_glyph_cache_release release) {
    if (!source || !width || !height || pitch < width ||
        width > SIZE_MAX / height || pitch > SIZE_MAX / height ||
        width * height > HMJE_GLYPH_CACHE_BYTES) return -1;
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = (hash ^ width) * UINT64_C(1099511628211);
    hash = (hash ^ height) * UINT64_C(1099511628211);
    /* Full content is still checked; eight-byte loads remove the byte-at-a-time
     * multiply dependency measured on scrolling. memcpy permits unaligned rows
     * and never reads row padding or past the caller's image. */
    for (uint64_t row = 0; row < height; ++row) {
        const unsigned char *line = source + row * pitch;
        uint64_t column = 0;
        for (; width - column >= 8; column += 8) {
            uint64_t word;
            memcpy(&word, line + column, sizeof(word));
            hash = (hash ^ word) * UINT64_C(1099511628211);
        }
        for (; column < width; ++column)
            hash = (hash ^ line[column]) * UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    unsigned first = (unsigned)(hash & (HMJE_GLYPH_CACHE_SLOTS / HMJE_GLYPH_CACHE_WAYS - 1))
                   * HMJE_GLYPH_CACHE_WAYS;
    unsigned victim = first;
    for (unsigned slot = first; slot < first + HMJE_GLYPH_CACHE_WAYS; ++slot) {
        hmje_glyph_cache_entry *entry = &cache->entries[slot];
        if (entry->bytes && entry->hash == hash && entry->width == width && entry->height == height) {
            int equal;
            if (pitch == width) {
                equal = memcmp(entry->bytes, source, (size_t)(width * height)) == 0;
            } else {
                equal = 1;
                for (uint64_t row = 0; row < height && equal; ++row)
                    equal = memcmp(entry->bytes + row * width, source + row * pitch, (size_t)width) == 0;
            }
            if (equal) {
                entry->used = ++cache->clock;
                return (int)slot;
            }
        }
        if (entry->used < cache->entries[victim].used) victim = slot;
    }
    size_t length = (size_t)(width * height);
    unsigned char *packed = malloc(length);
    if (!packed) return -1;
    for (uint64_t row = 0; row < height; ++row)
        memcpy(packed + row * width, source + row * pitch, (size_t)width);
    hmje_glyph_cache_drop(cache, victim, release);
    while (cache->bytes > HMJE_GLYPH_CACHE_BYTES - length) {
        unsigned oldest = HMJE_GLYPH_CACHE_SLOTS;
        for (unsigned slot = 0; slot < HMJE_GLYPH_CACHE_SLOTS; ++slot)
            if (cache->entries[slot].bytes && (oldest == HMJE_GLYPH_CACHE_SLOTS ||
                cache->entries[slot].used < cache->entries[oldest].used)) oldest = slot;
        if (oldest == HMJE_GLYPH_CACHE_SLOTS) break;
        hmje_glyph_cache_drop(cache, oldest, release);
    }
    cache->entries[victim] = (hmje_glyph_cache_entry){hash, width, height, ++cache->clock, packed};
    cache->bytes += length;
    return (int)victim;
}
#endif
