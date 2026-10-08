#include "external_url.h"
/* Platforms without a system browser hand-off report "not opened"; callers
 * keep showing the URL text so the document stays reachable. */
int64_t hmje_ui_open_external_url(const uint8_t *bytes, uint64_t length) {
    (void)bytes;
    (void)length;
    return 0;
}
