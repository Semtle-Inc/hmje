#ifndef HMJE_POSIX_HOST_IME_POLICY_H
#define HMJE_POSIX_HOST_IME_POLICY_H

/* Decide whether a keyDown that went through the input context must also be
 * queued as a physical key for HMJE's document editor.
 *
 * Observed with the macOS 2-Set Korean IME (tests/posix_host_text_input):
 *   ㅎ,ㅏ,ㄴ          -> setMarkedText("ㅎ"/"하"/"한")              : IME owns it
 *   Backspace in 한   -> setMarkedText("하")                        : IME owns it
 *   Left       -> insertText("한") + doCommandBySelector(...) : commit, then
 *                        the key must still move the caret
 *   Backspace in ㅎ   -> insertText("ㅎ") + deleteBackward:          : commit, then
 *                        the key must still delete it
 * The caller tags Return that commits marked text with the composition
 * flag. Enter actions consume it; ordinary inputs keep their key behaviour.
 * So an explicit command request forwards the key unless marked text remains
 * (e.g. a Japanese IME moving inside its clause). Committed text is queued
 * before the key, so HMJE sees "commit, then command". Keys that only marked
 * or committed text, or that the input context consumed silently, stay
 * inside the IME. */
static inline int hmje_posix_ime_forward_key(int was_marked, int is_marked,
                                             int committed, int mark_changed,
                                             int handled, int command_requested) {
    if (is_marked) return 0;
    if (command_requested) return 1;
    if (was_marked || committed || mark_changed) return 0;
    return handled ? 0 : 1;
}

#endif
