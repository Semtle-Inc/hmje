#ifndef HMJE_TEXT_INPUT_CONTROL_H
#define HMJE_TEXT_INPUT_CONTROL_H
#include <stdint.h>
/* SYS_TEXT_INPUT_CONTROL2 (111), syscall3(version, client, flags).
 * 104 retains its packed-client ABI. 110 is system appearance. */
#define HMJE_TEXT_INPUT_ACTIVE UINT64_C(1)
#define HMJE_TEXT_INPUT_IGNORE_SAFE_AREA UINT64_C(2)
#define HMJE_TEXT_INPUT_SECURE UINT64_C(4)
#define HMJE_TEXT_INPUT_MULTILINE UINT64_C(8)
/* Email field: email keyboard, no autocorrect/autocapitalize/smart punctuation. Platforms
 * without keyboard traits simply ignore it. */
#define HMJE_TEXT_INPUT_EMAIL UINT64_C(16)
/* SYS_TEXT_INPUT_CONTROL2 with a1 = 2 reports where the UI drew the IME
 * composition (or the caret): a2 = x | y << 32, a3 = width | height << 32, in
 * logical units (surface pixels / scale) with a top-left origin. Hosts place
 * the candidate/composition window there; hosts without IME windows ignore it. */
#define HMJE_TEXT_INPUT_CARET_RECT 2
static inline int hmje_text_input_validate(uint64_t version, uint64_t client, uint64_t flags) {
    if (version != 1) return -38;
    if ((flags & ~UINT64_C(31)) || ((flags & HMJE_TEXT_INPUT_ACTIVE) && !client)) return -22;
    return 0;
}
#endif
