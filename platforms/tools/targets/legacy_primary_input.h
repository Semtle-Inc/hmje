#ifndef HMJE_LEGACY_PRIMARY_INPUT_H
#define HMJE_LEGACY_PRIMARY_INPUT_H

/* iOS UIKit touch stream -> legacy single-pointer lifecycle adapter.
 *
 * The detailed pointer queue keeps every UIKit contact and is the source of
 * truth for multi-touch.  The legacy queue, however, can represent only one
 * pressed pointer.  Waiting for UIEvent.allTouches to prove that the previous
 * UITouch disappeared is unsafe: UIKit/Simulator can retain or replace touch
 * wrappers across a cancellation boundary, leaving the legacy primary stuck
 * forever.
 *
 * Therefore every DOWN is an unconditional legacy sequence boundary.  If a
 * primary is already recorded, synthesize its MOVE+UP at its last known
 * coordinate, then adopt and press the new contact.  This is also deterministic
 * for real multi-touch (latest DOWN wins in the legacy projection) and does not
 * alter the independent detailed records.
 */

#include <stdint.h>

#include "pointer_input.h"

#define HMJE_LEGACY_PRIMARY_EVENT_MOVE 1u
#define HMJE_LEGACY_PRIMARY_EVENT_BUTTON_DOWN 2u
#define HMJE_LEGACY_PRIMARY_EVENT_BUTTON_UP 3u

typedef struct hmje_legacy_primary_state {
    uintptr_t pointer_id;
    uint64_t x;
    uint64_t y;
    uint64_t forced_release_count;
    uint64_t wrapper_recovery_count;
} hmje_legacy_primary_state;

typedef void (*hmje_legacy_primary_emit_fn)(
    void *context,
    uint32_t event,
    uint64_t x,
    uint64_t y
);

static inline void hmje_legacy_primary_emit_position(
    hmje_legacy_primary_emit_fn emit,
    void *context,
    uint64_t x,
    uint64_t y
) {
    if (emit != NULL) emit(context, HMJE_LEGACY_PRIMARY_EVENT_MOVE, x, y);
}

static inline void hmje_legacy_primary_emit_button(
    hmje_legacy_primary_emit_fn emit,
    void *context,
    uint32_t event,
    uint64_t x,
    uint64_t y
) {
    if (emit != NULL) emit(context, event, x, y);
}

static inline void hmje_legacy_primary_clear(hmje_legacy_primary_state *state) {
    state->pointer_id = 0u;
    state->x = 0u;
    state->y = 0u;
}

/* tracked_primary_is_active is consulted only for a terminal callback whose
 * wrapper identity does not match.  DOWN intentionally never consults it: a
 * new DOWN must always be able to recover a lost legacy UP/CANCEL. */
static inline void hmje_legacy_primary_route(
    hmje_legacy_primary_state *state,
    uintptr_t pointer_id,
    uint64_t phase,
    uint64_t x,
    uint64_t y,
    int tracked_primary_is_active,
    hmje_legacy_primary_emit_fn emit,
    void *context
) {
    if (phase == HMJE_POINTER_PHASE_DOWN) {
        if (state->pointer_id != 0u) {
            hmje_legacy_primary_emit_position(emit, context, state->x, state->y);
            hmje_legacy_primary_emit_button(
                emit, context, HMJE_LEGACY_PRIMARY_EVENT_BUTTON_UP, state->x, state->y
            );
            state->forced_release_count += 1u;
        }

        state->pointer_id = pointer_id;
        state->x = x;
        state->y = y;
        hmje_legacy_primary_emit_position(emit, context, x, y);
        hmje_legacy_primary_emit_button(
            emit, context, HMJE_LEGACY_PRIMARY_EVENT_BUTTON_DOWN, x, y
        );
        return;
    }

    if (state->pointer_id != 0u && pointer_id == state->pointer_id) {
        state->x = x;
        state->y = y;
        hmje_legacy_primary_emit_position(emit, context, x, y);
        if (phase == HMJE_POINTER_PHASE_UP || phase == HMJE_POINTER_PHASE_CANCEL) {
            hmje_legacy_primary_emit_button(
                emit, context, HMJE_LEGACY_PRIMARY_EVENT_BUTTON_UP, x, y
            );
            hmje_legacy_primary_clear(state);
        }
        return;
    }

    if ((phase == HMJE_POINTER_PHASE_UP || phase == HMJE_POINTER_PHASE_CANCEL) &&
        state->pointer_id != 0u && !tracked_primary_is_active) {
        /* UIKit may use a different wrapper for the final callback.  When the
         * tracked wrapper is no longer active, this terminal sample closes the
         * single-pointer projection at the terminal coordinate. */
        state->x = x;
        state->y = y;
        hmje_legacy_primary_emit_position(emit, context, x, y);
        hmje_legacy_primary_emit_button(
            emit, context, HMJE_LEGACY_PRIMARY_EVENT_BUTTON_UP, x, y
        );
        state->wrapper_recovery_count += 1u;
        hmje_legacy_primary_clear(state);
    }
}

#endif
