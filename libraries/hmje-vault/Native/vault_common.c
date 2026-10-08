#include "vault.h"
int64_t hmje_vault_utf8_valid(const void *data, int64_t size, int64_t allow_nul) {
    if (size < 0 || size > VAULT_VALUE_MAX || (size && !data))
        return 0;
    const unsigned char *p = data;
    size_t i = 0, n = (size_t)size;
    while (i < n) {
        uint32_t c = p[i++];
        unsigned extra = 0;
        uint32_t minimum = 0;
        if (c < 0x80) {
            if (c == 0 && !allow_nul)
                return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            extra = 1;
            minimum = 0x80;
            c &= 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            extra = 2;
            minimum = 0x800;
            c &= 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            extra = 3;
            minimum = 0x10000;
            c &= 7;
        } else
            return 0;
        if (n - i < extra)
            return 0;
        while (extra--) {
            unsigned char d = p[i++];
            if ((d & 0xc0) != 0x80)
                return 0;
            c = (c << 6) | (d & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return 0;
    }
    return 1;
}
int64_t hmje_vault_wipe(void *data, int64_t size) {
    if (!data || size <= 0)
        return 0;
    volatile unsigned char *p = data;
    while (size--)
        *p++ = 0;
    return 0;
}
