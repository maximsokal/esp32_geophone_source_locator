#include "scan_preview_state.h"

#include <string.h>

static void release_payload(scan_preview_state_t *state)
{
    scan_preview_free(&state->preview);
    scan_occupancy_free(&state->occupancy);
    state->has_preview = false;
    state->has_occupancy = false;
}

void scan_preview_state_init(scan_preview_state_t *state)
{
    if (state != NULL) memset(state, 0, sizeof(*state));
}

uint32_t scan_preview_state_open(scan_preview_state_t *state)
{
    if (state == NULL) return 0U;
    release_payload(state);
    state->generation++;
    if (state->generation == 0U) state->generation++;
    state->open = true;
    state->render_in_flight = false;
    state->status = SCAN_PREVIEW_SCREEN_LOADING;
    state->fallback_reason[0] = '\0';
    return state->generation;
}

void scan_preview_state_close(scan_preview_state_t *state)
{
    if (state == NULL) return;
    release_payload(state);
    state->generation++;
    if (state->generation == 0U) state->generation++;
    state->open = false;
    state->render_in_flight = false;
    state->status = SCAN_PREVIEW_SCREEN_LOADING;
    state->fallback_reason[0] = '\0';
}

bool scan_preview_state_complete(
    scan_preview_state_t *state,
    uint32_t generation,
    scan_preview_screen_status_t status,
    scan_preview_t *preview,
    scan_occupancy_t *occupancy,
    const char *fallback_reason
)
{
    const bool accepted = state != NULL && state->open && state->generation == generation;
    if (!accepted) {
        if (preview != NULL) scan_preview_free(preview);
        if (occupancy != NULL) scan_occupancy_free(occupancy);
        return false;
    }

    release_payload(state);
    if (status == SCAN_PREVIEW_SCREEN_READY) {
        if (preview != NULL && preview->allocation != NULL) {
            state->preview = *preview;
            memset(preview, 0, sizeof(*preview));
            state->has_preview = true;
        }
        if (occupancy != NULL && (occupancy->occupied_keys != NULL || occupancy->free_keys != NULL)) {
            state->occupancy = *occupancy;
            memset(occupancy, 0, sizeof(*occupancy));
            state->has_occupancy = true;
        }
        if (!state->has_preview && !state->has_occupancy) status = SCAN_PREVIEW_SCREEN_NO_PREVIEW;
    } else {
        if (preview != NULL) scan_preview_free(preview);
        if (occupancy != NULL) scan_occupancy_free(occupancy);
    }

    state->status = status;
    if (fallback_reason != NULL) {
        (void)strncpy(state->fallback_reason, fallback_reason, sizeof(state->fallback_reason) - 1U);
        state->fallback_reason[sizeof(state->fallback_reason) - 1U] = '\0';
    } else {
        state->fallback_reason[0] = '\0';
    }
    return true;
}

bool scan_preview_state_render_begin(scan_preview_state_t *state, uint32_t generation)
{
    if (state == NULL || !state->open || state->generation != generation ||
        state->status != SCAN_PREVIEW_SCREEN_READY || state->render_in_flight) return false;
    state->render_in_flight = true;
    return true;
}

void scan_preview_state_render_end(scan_preview_state_t *state, uint32_t generation)
{
    if (state != NULL && state->generation == generation) state->render_in_flight = false;
}
