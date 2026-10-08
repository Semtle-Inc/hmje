#include "file_dialog.h"

int64_t hmje_ui_file_dialog_supported(void) { return 0; }

int64_t hmje_ui_choose_file(int64_t action, uint8_t *output,
                            uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (action != 1 && action != 2)) return -1;
    return -2;
}

int64_t hmje_ui_choose_folder(const uint8_t *initial, uint64_t initial_length,
                              const uint8_t *prompt, uint64_t prompt_length,
                              uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (initial_length && !initial) ||
        (prompt_length && !prompt)) return -1;
    return -2;
}
