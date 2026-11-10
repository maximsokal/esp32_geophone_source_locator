#pragma once

#include <stdbool.h>
#include <stddef.h>


/*
 * Whether the recorder task may start its next pass at once after a pass that
 * did some work, or has to block for one tick first.
 *
 * The task runs at priority 3 on the service core and used to repeat without
 * blocking after every pass that moved at least one frame. taskYIELD() only
 * gives way to tasks of the same or a higher priority, so whenever frames
 * arrive faster than one pass takes, the task would never block: the nRF
 * topology, INA3221 and workflow tasks (priorities 1 and 2) and IDLE0 would
 * stop running, I2C transfers would time out and the task watchdog would
 * fire. At 500 Hz a pass is shorter than the 2 ms sample period, so this only
 * shows at higher sample rates.
 *
 * Catching up is still allowed without a pause while there is a real backlog:
 *  - STOP is draining the cache to the card;
 *  - the write-behind cache is full, so the realtime ring is the only buffer;
 *  - the pass took a full batch out of the realtime ring, so more is waiting;
 *  - after the pass the cache still holds a whole write batch of dirty frames.
 * Otherwise one tick of sleep costs nothing: the realtime ring holds seconds
 * of samples and the next pass takes everything that arrived meanwhile.
 */
static inline bool seismic_recorder_pass_may_repeat_without_blocking(
    bool stop_requested,
    bool cache_full,
    size_t ingested_frames,
    size_t ingest_batch_frames,
    size_t dirty_frames,
    size_t write_batch_frames
)
{
    if (stop_requested || cache_full) {
        return true;
    }
    if (ingest_batch_frames > 0U && ingested_frames >= ingest_batch_frames) {
        return true;
    }
    return write_batch_frames > 0U && dirty_frames >= write_batch_frames;
}
