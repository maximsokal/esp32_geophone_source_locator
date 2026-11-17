#pragma once

#include <stdint.h>

/*
 * The timestamps of the synthetic source (TEST generator, used by the emulator scenarios and bench tests).
 *
 * Three consumers set three rules, and only one design satisfies all of them:
 *   1. strictly increasing: the calculation rejects a trace that stands still or steps back;
 *   2. exactly one frame period apart: the V2 samples file stores one timestamp per run of consecutive frames and the reader
 *      rebuilds the others from the sample rate; stamps that drift from the grid make a later read fail
 *      (emulator S10: "LOC_SEARCH ... err=ESP_ERR_INVALID_RESPONSE" after stamping with steps of 1 us);
 *   3. never later than the clock: the recorder refuses to start on a sample "from the future"
 *      (seismic_recorder_start_validate_acquisition_at: now_us < last_sample_timestamp_us; emulator S10 once saw the last
 *      sample 8.8 ms ahead, "START rejected: acquisition sample is not fresh").
 *
 * Rules 2 and 3 clash when frames are produced faster than the clock moves (emulation: ticks come early). The way out is
 * not to produce them faster: the stamps run on a grid, one period apart, from a fixed origin, and a tick of frames is
 * pushed only once the clock has reached the last stamp of the tick. On the board the tick is exact and the wait is nil.
 * After a long stall the grid is moved forward to the clock (a gap in time, the frame counter goes on), so the samples do
 * not stay stale for ever.
 */

/* The stamp of the last frame of a tick that starts at `first_stamp_us`: the clock must have reached it. */
static inline int64_t seismic_test_stamp_ready_at(int64_t first_stamp_us, int frames_per_tick, int64_t frame_period_us)
{
    return first_stamp_us + (int64_t)(frames_per_tick - 1) * frame_period_us;
}

/*
 * The stamp of the first frame of the next tick. `next_stamp_us` is the grid's next free stamp (0 before the first tick).
 * The first tick, and a tick that is more than `reanchor_after_us` late, start so that the last frame is stamped at the
 * clock; every other tick continues the grid exactly.
 */
static inline int64_t seismic_test_stamp_first(
    int64_t next_stamp_us,
    int frames_per_tick,
    int64_t frame_period_us,
    int64_t clock_us,
    int64_t reanchor_after_us)
{
    const int64_t anchored_us = clock_us - (int64_t)(frames_per_tick - 1) * frame_period_us;
    if (next_stamp_us == 0) return anchored_us;
    const int64_t ready_us = seismic_test_stamp_ready_at(next_stamp_us, frames_per_tick, frame_period_us);
    return clock_us - ready_us > reanchor_after_us ? anchored_us : next_stamp_us;
}
