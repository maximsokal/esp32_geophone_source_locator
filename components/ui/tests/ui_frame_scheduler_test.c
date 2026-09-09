#include "ui_frame_scheduler.h"

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
    puts("ui frame scheduler tests passed");
    return 0;
}
