#ifndef HMJE_EXTERNAL_URL_H
#define HMJE_EXTERNAL_URL_H
#include <stdint.h>
/* Hands one absolute http(s) URL (UTF-8, not NUL-terminated) to the platform
 * browser. Returns 1 when the request was dispatched, 0 when the URL was
 * rejected or the platform has no browser. Never blocks on the browser. */
int64_t hmje_ui_open_external_url(const uint8_t *bytes, uint64_t length);
#endif
