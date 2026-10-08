#include "gpu_glyph_cache.h"
static hmje_glyph_cache g_metal_glyph_cache;
static id<MTLTexture> g_metal_glyph_textures[HMJE_GLYPH_CACHE_SLOTS];
static id<MTLDevice> g_metal_glyph_device;
static uint64_t g_metal_glyph_uploads, g_metal_glyph_hits;
/* Address front cache. The content cache hashes every glyph bitmap on every
 * draw (a multiply chain over each 8-byte word) before the full compare; a
 * frame draws the same few hundred glyphs thousands of times from the same
 * atlas addresses. This table remembers which slot an address last resolved
 * to, and a hit still compares the full bitmap against the slot's owned copy,
 * so a moved or reused atlas address can only miss, never draw a wrong glyph.
 * A slot's serial changes whenever its content is dropped. */
#define HMJE_METAL_GLYPH_FRONT_SLOTS 4096u
typedef struct {
    const unsigned char *source;
    uint64_t width, height, pitch, serial;
    int slot;
} hmje_metal_glyph_front_entry;
static hmje_metal_glyph_front_entry g_metal_glyph_front[HMJE_METAL_GLYPH_FRONT_SLOTS];
static uint64_t g_metal_glyph_slot_serial[HMJE_GLYPH_CACHE_SLOTS];
static uint64_t g_metal_glyph_front_hits;
static void hmje_metal_glyph_release(unsigned slot) {
    /* Command buffers/transaction resources retain immutable textures while
     * they are in flight. Eviction only removes the cache's reference. */
    g_metal_glyph_textures[slot] = nil;
    ++g_metal_glyph_slot_serial[slot];
}
static unsigned hmje_metal_glyph_front_index(const unsigned char *source, uint64_t width,
                                             uint64_t height, uint64_t pitch) {
    uint64_t key = (uint64_t)(uintptr_t)source * UINT64_C(0x9e3779b97f4a7c15);
    key ^= (width << 1) ^ (height << 17) ^ (pitch << 33);
    key ^= key >> 29;
    return (unsigned)(key & (HMJE_METAL_GLYPH_FRONT_SLOTS - 1));
}
static id<MTLTexture> hmje_metal_glyph_front_lookup(const unsigned char *source, uint64_t width,
                                                    uint64_t height, uint64_t pitch) {
    hmje_metal_glyph_front_entry *front =
        &g_metal_glyph_front[hmje_metal_glyph_front_index(source, width, height, pitch)];
    if (front->source != source || front->width != width || front->height != height ||
        front->pitch != pitch || front->slot < 0) return nil;
    unsigned slot = (unsigned)front->slot;
    if (slot >= HMJE_GLYPH_CACHE_SLOTS || g_metal_glyph_slot_serial[slot] != front->serial) return nil;
    hmje_glyph_cache_entry *entry = &g_metal_glyph_cache.entries[slot];
    id<MTLTexture> texture = g_metal_glyph_textures[slot];
    if (texture == nil || entry->bytes == NULL || entry->width != width || entry->height != height) return nil;
    for (uint64_t row = 0; row < height; ++row)
        if (memcmp(entry->bytes + row * width, source + row * pitch, (size_t)width) != 0) return nil;
    entry->used = ++g_metal_glyph_cache.clock;
    return texture;
}
static void hmje_metal_glyph_front_store(const unsigned char *source, uint64_t width,
                                         uint64_t height, uint64_t pitch, int slot) {
    if (slot < 0) return;
    hmje_metal_glyph_front_entry *front =
        &g_metal_glyph_front[hmje_metal_glyph_front_index(source, width, height, pitch)];
    *front = (hmje_metal_glyph_front_entry){source, width, height, pitch,
                                            g_metal_glyph_slot_serial[slot], slot};
}
static id<MTLTexture> hmje_metal_glyph_texture(id<MTLDevice> device,
    const unsigned char *source, uint64_t width, uint64_t height, uint64_t pitch) {
    if (!device || !source || !width || !height || pitch < width ||
        width > NSUIntegerMax / height || pitch > NSUIntegerMax / height) return nil;
    if (g_metal_glyph_device != device) {
        hmje_glyph_cache_clear(&g_metal_glyph_cache, hmje_metal_glyph_release);
        g_metal_glyph_device = device;
    }
    id<MTLTexture> front = hmje_metal_glyph_front_lookup(source, width, height, pitch);
    if (front != nil) {
        ++g_metal_glyph_front_hits;
        return front;
    }
    int slot = hmje_glyph_cache_get(&g_metal_glyph_cache, source, width, height, pitch,
                                   hmje_metal_glyph_release);
    if (slot >= 0 && g_metal_glyph_textures[slot] != nil) {
        ++g_metal_glyph_hits;
        hmje_metal_glyph_front_store(source, width, height, pitch, slot);
        return g_metal_glyph_textures[slot];
    }
    MTLTextureDescriptor *description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
        width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
    description.usage = MTLTextureUsageShaderRead;
    description.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:description];
    if (texture == nil) {
        if (slot >= 0) hmje_glyph_cache_drop(&g_metal_glyph_cache, (unsigned)slot,
                                            hmje_metal_glyph_release);
        return nil;
    }
    const void *bytes = slot >= 0 ? g_metal_glyph_cache.entries[slot].bytes : source;
    [texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
               mipmapLevel:0 withBytes:bytes bytesPerRow:(NSUInteger)(slot >= 0 ? width : pitch)];
    if (slot >= 0) {
        g_metal_glyph_textures[slot] = texture;
        hmje_metal_glyph_front_store(source, width, height, pitch, slot);
    }
    ++g_metal_glyph_uploads;
    return texture;
}
