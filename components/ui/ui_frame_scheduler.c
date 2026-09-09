#include "ui_frame_scheduler.h"

#include <stddef.h>

void ui_frame_scheduler_init(ui_frame_scheduler_t *scheduler, uint64_t now_us)
{
    if (scheduler == NULL) return;
    *scheduler = (ui_frame_scheduler_t) { .next_due_us = now_us };
}

void ui_frame_scheduler_submit(ui_frame_scheduler_t *scheduler, uint64_t generation)
{
    if (scheduler == NULL) return;
    if (scheduler->pending && generation != scheduler->latest_generation) {
        ++scheduler->frames_coalesced;
    }
    scheduler->latest_generation = generation;
    scheduler->pending = true;
}

bool ui_frame_scheduler_take(ui_frame_scheduler_t *scheduler, uint64_t now_us, uint64_t *generation)
{
    if (scheduler == NULL || !scheduler->pending || now_us < scheduler->next_due_us) return false;
    scheduler->pending = false;
    scheduler->rendered_generation = scheduler->latest_generation;
    ++scheduler->frames_rendered;
    if (generation != NULL) *generation = scheduler->rendered_generation;
    /* Schedule from now: a stall never creates catch-up render debt. */
    scheduler->next_due_us = now_us + UI_FRAME_MIN_PERIOD_US;
    return true;
}
