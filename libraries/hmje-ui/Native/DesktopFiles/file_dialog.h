#ifndef HMJE_UI_FILE_DIALOG_H
#define HMJE_UI_FILE_DIALOG_H
#include <stdint.h>

/* Synchronous UI-thread picker. action: 1=open, 2=save.
 * Returns 1=selected, 0=cancelled, -1=bad arguments, -2=platform error or
 * unavailable, -3=path exceeds capacity. On every non-success, *written=0
 * and output bytes are not modified. Selected path is absolute UTF-8 with no
 * trailing NUL; caller owns its buffer. No pointer is retained by the picker. */
int64_t hmje_ui_choose_file(int64_t action, uint8_t *output,
                            uint64_t capacity, uint64_t *written);
/* Whether this target has a desktop file picker, not a permission guarantee. */
int64_t hmje_ui_file_dialog_supported(void);
/* Optional UTF-8 initial absolute directory and prompt (length-delimited).
 * Result and status follow hmje_ui_choose_file. */
int64_t hmje_ui_choose_folder(const uint8_t *initial, uint64_t initial_length,
                              const uint8_t *prompt, uint64_t prompt_length,
                              uint8_t *output, uint64_t capacity, uint64_t *written);
#endif
