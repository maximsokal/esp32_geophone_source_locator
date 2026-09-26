#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "scan_occupancy.h"
#include "scan_preview.h"

typedef enum
{
    SCAN_PREVIEW_SCREEN_LOADING = 0,
    SCAN_PREVIEW_SCREEN_NO_3D_SCAN,
    SCAN_PREVIEW_SCREEN_NO_PREVIEW,
    SCAN_PREVIEW_SCREEN_READY,
} scan_preview_screen_status_t;

typedef struct
{
    uint32_t generation;
    bool open;
    bool render_in_flight;
    scan_preview_screen_status_t status;
    scan_preview_t preview;
    scan_occupancy_t occupancy;
    bool has_preview;
    bool has_occupancy;
    char fallback_reason[40];
} scan_preview_state_t;

void scan_preview_state_init(scan_preview_state_t *state);
uint32_t scan_preview_state_open(scan_preview_state_t *state);
void scan_preview_state_close(scan_preview_state_t *state);

/* Always consumes and clears both result arguments, including stale results. */
bool scan_preview_state_complete(
    scan_preview_state_t *state,
    uint32_t generation,
    scan_preview_screen_status_t status,
    scan_preview_t *preview,
    scan_occupancy_t *occupancy,
    const char *fallback_reason
);

bool scan_preview_state_render_begin(scan_preview_state_t *state, uint32_t generation);
void scan_preview_state_render_end(scan_preview_state_t *state, uint32_t generation);
