#include "ui_frame_scheduler.h"
#include "ui_event_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    ui_frame_scheduler_t scheduler;
    ui_frame_scheduler_init(&scheduler, 0U);
    uint64_t rendered = 0U;
    unsigned callbacks = 0U;

    for (uint64_t sample = 1U; sample <= 5000U; ++sample) {
        const uint64_t now = sample * 2000U;
        ui_frame_scheduler_submit(&scheduler, sample);
        if (ui_frame_scheduler_take(&scheduler, now, &rendered)) ++callbacks;
    }
    assert(callbacks <= 241U);
    assert(rendered <= 5000U);

    ui_frame_scheduler_init(&scheduler, 0U);
    for (uint64_t state = 1U; state <= 250U; ++state) ui_frame_scheduler_submit(&scheduler, state);
    assert(ui_frame_scheduler_take(&scheduler, 500000U, &rendered));
    assert(rendered == 250U);
    assert(!ui_frame_scheduler_take(&scheduler, 500000U, &rendered));
    assert(scheduler.frames_coalesced == 249U);

    ui_frame_scheduler_submit(&scheduler, 251U);
    assert(!ui_frame_scheduler_take(&scheduler, 541666U, &rendered));
    assert(ui_frame_scheduler_take(&scheduler, 541667U, &rendered));
    assert(rendered == 251U);

    ui_frame_scheduler_init(&scheduler, 0U);
    ui_frame_scheduler_submit(&scheduler, 1U);
    assert(ui_frame_scheduler_take(&scheduler, 0U, &rendered));
    ui_frame_scheduler_submit(&scheduler, 21U);
    assert(scheduler.frames_coalesced == 19U);
    assert(ui_frame_scheduler_take(&scheduler, UI_FRAME_MIN_PERIOD_US, &rendered));
    assert(rendered == 21U);

    /* A navigation press delayed by exactly the budget is still current; one
     * microsecond beyond it is stale and must never be replayed as a later
     * menu step. */
    const int64_t queued_us = 1000000LL;
    assert(!ui_event_keypad_is_stale(queued_us, queued_us));
    assert(!ui_event_keypad_is_stale(
        queued_us,
        queued_us + UI_KEYPAD_EVENT_MAX_AGE_US
    ));
    assert(ui_event_keypad_is_stale(
        queued_us,
        queued_us + UI_KEYPAD_EVENT_MAX_AGE_US + 1LL
    ));
    assert(!ui_event_keypad_is_stale(queued_us, queued_us - 1LL));

    puts("ui frame scheduler and event freshness tests passed");
    return 0;
}
