#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "seismic_recorder_pacing.h"
#include "seismic_recorder_start_policy.h"


int64_t esp_timer_get_time(void)
{
    return 1000000;
}


/* 512-frame ingest batch and a 1000-frame write batch, as at 2 kHz. */
static bool repeats(bool stop, bool cache_full, size_t ingested, size_t dirty)
{
    return seismic_recorder_pass_may_repeat_without_blocking(
        stop, cache_full, ingested, 512U, dirty, 1000U);
}


static void test_recorder_pass_blocks_unless_there_is_a_backlog(void)
{
    /* The starvation case: frames keep arriving, a few per pass, nothing else is pending.
     * The task has to block or priorities 1-2 and IDLE0 never run on the service core. */
    assert(!repeats(false, false, 1U, 0U));
    assert(!repeats(false, false, 20U, 999U));
    assert(!repeats(false, false, 511U, 0U));
    assert(!repeats(false, false, 0U, 0U));       /* a pass that only wrote */

    /* A full batch came out of the realtime ring: more is waiting there. */
    assert(repeats(false, false, 512U, 0U));
    assert(repeats(false, false, 600U, 0U));
    /* A whole write batch is still dirty after the pass. */
    assert(repeats(false, false, 1U, 1000U));
    assert(repeats(false, false, 0U, 5000U));
    /* The cache is full, or STOP drains it: never slowed down. */
    assert(repeats(false, true, 0U, 0U));
    assert(repeats(true, false, 0U, 0U));

    /* Zero batch sizes are not a backlog. */
    assert(!seismic_recorder_pass_may_repeat_without_blocking(false, false, 0U, 0U, 0U, 0U));
    assert(!seismic_recorder_pass_may_repeat_without_blocking(false, false, 7U, 0U, 7U, 0U));
}


int main(void)
{
    test_recorder_pass_blocks_unless_there_is_a_backlog();
    seismic_acquisition_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.running = true;
    snapshot.source_healthy = false;
    snapshot.sample_rate_hz = 500U;
    snapshot.latest_valid = true;
    snapshot.samples = 1U;
    snapshot.last_sample_timestamp_us = 999000;

    /* The exact regression: stale BUSY presence must not reject START. */
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_OK);
    assert(
        seismic_recorder_start_validate_acquisition_at(&snapshot, 1000000) ==
        ESP_OK
    );

    snapshot.running = false;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.running = true;
    snapshot.sample_rate_hz = 0U;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.sample_rate_hz = 500U;
    snapshot.latest_valid = false;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.latest_valid = true;
    snapshot.samples = 0U;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.samples = 1U;
    snapshot.last_sample_timestamp_us = 1000000 - (2000 * 64) - 1;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.last_sample_timestamp_us = 1000001;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.last_sample_timestamp_us = 999000;
    assert(seismic_recorder_start_validate_acquisition_at(&snapshot, -1) == ESP_ERR_INVALID_STATE);
    assert(seismic_recorder_start_validate_acquisition(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_start_validate_acquisition_at(NULL, 1000000) == ESP_ERR_INVALID_ARG);
    return 0;
}
