#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_FRAME_TARGET_FPS       24U
#define UI_FRAME_MIN_PERIOD_US    41667ULL

typedef struct {
    uint64_t next_due_us;
    uint64_t latest_generation;
    uint64_t rendered_generation;
    uint64_t frames_rendered;
    uint64_t frames_coalesced;
    bool pending;
} ui_frame_scheduler_t;

void ui_frame_scheduler_init(ui_frame_scheduler_t *scheduler, uint64_t now_us);
void ui_frame_scheduler_submit(ui_frame_scheduler_t *scheduler, uint64_t generation);
bool ui_frame_scheduler_take(ui_frame_scheduler_t *scheduler, uint64_t now_us, uint64_t *generation);
