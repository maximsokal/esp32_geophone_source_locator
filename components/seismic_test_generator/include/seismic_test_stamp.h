#pragma once

#include <stdint.h>

/*
 * The timestamp of one frame of the synthetic source (TEST generator, used by the emulator scenarios).
 *
 * Two rules, both enforced by the consumers:
 *   1. strictly increasing: the calculation rejects a trace that stands still or steps back;
 *   2. never later than the clock: the recorder refuses to start on a sample "from the future"
 *      (seismic_recorder_start_validate_acquisition_at: now_us < last_sample_timestamp_us).
 *
 * A tick normally stamps its frames from `tick_us` backwards, one period apart, so the last one is the tick itself. When
 * ticks come closer together than the clock moves (emulation), that would not be later than the previous frame: the old
 * code then added a whole period to the previous stamp, which ran ahead of the clock (emulator S10: last sample 8.8 ms
 * ahead, "START rejected: acquisition sample is not fresh"). Here the step is only as large as the clock allows, 1 us at
 * least. If the clock itself has not moved past the previous stamp, rule 1 wins by 1 us.
 */
static inline int64_t seismic_test_stamp_next(
    int64_t tick_us,
    int frames_per_tick,
    int frame_index,
    int64_t frame_period_us,
    int64_t last_stamp_us,
    int64_t clock_us)
{
    int64_t stamp_us = tick_us - (int64_t)(frames_per_tick - 1 - frame_index) * frame_period_us;
    if (last_stamp_us != 0 && stamp_us <= last_stamp_us) {
        const int64_t room_us = clock_us - last_stamp_us;
        const int64_t step_us = room_us > 1 ? (room_us < frame_period_us ? room_us : frame_period_us) : 1;
        stamp_us = last_stamp_us + step_us;
    }
    return stamp_us;
}
