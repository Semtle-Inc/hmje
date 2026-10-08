#ifndef HMJE_POSIX_HOST_KEYMAP_H
#define HMJE_POSIX_HOST_KEYMAP_H

/* Native macOS key codes share numbers with the guest's set-1 command keys.
 * Only semantic editing/navigation keys retain their code. All other hosted
 * keys move into a disjoint range; committed text travels through TEXT events. */
static inline unsigned short hmje_posix_normalize_key_code(unsigned short key_code) {
    /* F2 toggles the HmjeUI frame profiler (set-1 scan code 60) like the
     * other desktop hosts. Normalized codes below 128 are otherwise unused. */
    if (key_code == 120) return 60; /* F2 */
    if (key_code == 48) return 250; /* Tab */
    if (key_code == 36) return 251; /* Return */
    if (key_code == 49) return 252; /* Space */
    if (key_code == 51 || key_code == 53 || key_code == 115 || key_code == 116 ||
        key_code == 117 || key_code == 119 || key_code == 121 ||
        (key_code >= 123 && key_code <= 126)) {
        return key_code; /* delete, escape, navigation */
    }
    if (key_code <= 121) return (unsigned short)(128 + key_code);
    if (key_code <= 127) return (unsigned short)(240 + key_code - 122);
    return key_code;
}

/* Command shortcuts are identified by the letter the user sees on an ASCII
 * layout. Non-Latin input sources (2-Set Korean reports Cmd+Z as "ㅋ" in
 * charactersIgnoringModifiers) fall back to the physical ANSI position, the
 * same way AppKit key equivalents keep working under those sources. Latin
 * layouts such as Dvorak keep their own letters. Returns 0 when there is no
 * letter. */
static inline char hmje_posix_shortcut_letter(unsigned short key_code, unsigned int ignoring_first) {
    if (ignoring_first >= 'A' && ignoring_first <= 'Z') return (char)(ignoring_first - 'A' + 'a');
    if (ignoring_first >= 'a' && ignoring_first <= 'z') return (char)ignoring_first;
    if (ignoring_first != 0 && ignoring_first < 128) return 0; /* digits, punctuation */
    static const char ansi[51] = {
        'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
        'y', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'o',
        'u', 0, 'i', 'p', 0, 'l', 'j', 0, 'k', 0, 0, 0, 0, 'n', 'm', 0,
        0, 0, 0,
    };
    return key_code < sizeof ansi ? ansi[key_code] : 0;
}

#endif
