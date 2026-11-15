/* MAX-65 autonomous recorder failure contract. Reuse the established worker
 * harness so this slice exercises production measurement_service.c with the
 * same FreeRTOS/object/recorder boundary fakes as the default host test. */
#define main measurement_service_worker_contract_existing_main
#include "measurement_service_worker_contract_test.c"
#undef main


static unsigned s_autonomous_refresh_calls;
extern uint32_t measurement_source_rate_hz;


static void autonomous_storage_refresh(void *user_context)
{
    assert(user_context == &s_autonomous_refresh_calls);
    ++s_autonomous_refresh_calls;
}


static void test_recording_poll_wait_policy(void)
{
    assert(
        measurement_service_worker_wait_ticks(MEASUREMENT_SERVICE_RECORDING) ==
        pdMS_TO_TICKS(MEASUREMENT_SERVICE_RECORDING_POLL_MS)
    );
    assert(
        measurement_service_worker_wait_ticks(MEASUREMENT_SERVICE_STOPPING) ==
        pdMS_TO_TICKS(MEASUREMENT_SERVICE_STOPPING_POLL_MS)
    );
    assert(
        measurement_service_worker_wait_ticks(MEASUREMENT_SERVICE_LIVE) ==
        pdMS_TO_TICKS(1000U)
    );
    assert(
        measurement_service_worker_wait_ticks(MEASUREMENT_SERVICE_REVIEW) ==
        pdMS_TO_TICKS(1000U)
    );
    assert(
        measurement_service_worker_wait_ticks(MEASUREMENT_SERVICE_ERROR) ==
        pdMS_TO_TICKS(1000U)
    );
}


static void test_autonomous_recorder_error_converges_after_finalization(void)
{
    start_service();
    s_autonomous_refresh_calls = 0U;
    assert(
        measurement_service_set_storage_refresh_callback(
            autonomous_storage_refresh,
            &s_autonomous_refresh_calls
        ) == ESP_OK
    );

    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);

    /* Recorder independently encounters an I/O error and owns finalization.
     * Measurement must stop advertising RECORDING before the file closes, but
     * must not publish storage refresh while metadata is still in flight. */
    fake_recorder.running = true;
    fake_recorder.state = SEISMIC_RECORDER_STOPPING;
    fake_recorder.last_error = ESP_FAIL;
    fake_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_SYNC;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    fake_recorder.stop_stage_error = ESP_FAIL;
    fake_recorder.samples_written = 27U;
    fake_recorder.dropped_frames = 4U;
    fake_recorder.recording_ms = 910U;

    measurement_service_process_stop_poll();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_STOPPING);
    assert(current.last_error == ESP_FAIL);
    assert(current.stop_stage == SEISMIC_RECORDER_STOP_STAGE_SYNC);
    assert(current.samples_written == 27U);
    assert(current.dropped_frames == 4U);
    assert(s_autonomous_refresh_calls == 0U);
    assert(!measurement_service_snapshot_allows_object_mutation(&current));
    assert(measurement_service_snapshot_blocks_acquisition_stop(&current));
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);

    /* The recorder's terminal ERROR is published only after final metadata is
     * complete. That exact transition releases control admission and requests
     * one cache refresh for the committed error metadata. */
    fake_recorder.running = false;
    fake_recorder.state = SEISMIC_RECORDER_ERROR;
    fake_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_ERROR;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    fake_recorder.stop_stage_error = ESP_FAIL;
    fake_recorder.samples_written = 31U;
    fake_recorder.dropped_frames = 5U;
    fake_recorder.recording_ms = 1040U;

    measurement_service_process_stop_poll();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_FAIL);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.session_id, "S000001") == 0);
    assert(current.stop_stage == SEISMIC_RECORDER_STOP_STAGE_ERROR);
    assert(current.stop_error_stage == SEISMIC_RECORDER_STOP_STAGE_DRAIN);
    assert(current.stop_stage_error == ESP_FAIL);
    assert(current.samples_written == 31U);
    assert(current.dropped_frames == 5U);
    assert(current.recording_ms == 1040U);
    assert(s_autonomous_refresh_calls == 1U);
    assert(measurement_service_snapshot_allows_object_mutation(&current));
    assert(!measurement_service_snapshot_blocks_acquisition_stop(&current));

    /* Terminal state is no longer polled, so refresh remains exactly once. */
    measurement_service_process_stop_poll();
    assert(s_autonomous_refresh_calls == 1U);
    stop_service();
}


static void test_successful_stop_refresh_remains_single(void)
{
    start_service();
    s_autonomous_refresh_calls = 0U;
    assert(
        measurement_service_set_storage_refresh_callback(
            autonomous_storage_refresh,
            &s_autonomous_refresh_calls
        ) == ESP_OK
    );

    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);
    assert(s_autonomous_refresh_calls == 0U);

    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    assert(snapshot().state == MEASUREMENT_SERVICE_REVIEW);
    assert(s_autonomous_refresh_calls == 1U);

    measurement_service_process_stop_poll();
    assert(s_autonomous_refresh_calls == 1U);
    stop_service();
}


/* A recording at a high rate ends by itself when the time its rate allows is up (seismic_rate_profile.h). */
static void test_recording_stops_itself_at_the_rate_limit(void)
{
    const struct { uint32_t rate_hz; uint64_t limit_ms; } cases[] = {
        { 2000U, 120000U }, { 4000U, 30000U }, { 5000U, 25000U },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        start_service();
        measurement_source_rate_hz = cases[i].rate_hz;
        assert(measurement_service_start_async() == ESP_OK);
        run_pending_command();
        assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
        assert(snapshot().record_limit_ms == cases[i].limit_ms);
        assert(!snapshot().stopped_by_limit);

        /* One poll short of the limit: still recording, no stop queued. */
        fake_recorder.recording_ms = cases[i].limit_ms - 1U;
        measurement_service_process_stop_poll();
        measurement_service_enforce_record_limit();
        assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
        assert(!snapshot().stopped_by_limit);

        /* At the limit: the ordinary STOP is requested, and the snapshot says why. */
        fake_recorder.recording_ms = cases[i].limit_ms;
        measurement_service_process_stop_poll();
        measurement_service_enforce_record_limit();
        assert(snapshot().stopped_by_limit);
        run_pending_command();
        assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);
        assert(seismic_recorder_wait_for_stop() == ESP_OK);
        measurement_service_process_stop_poll();
        assert(snapshot().state == MEASUREMENT_SERVICE_REVIEW);
        assert(snapshot().stopped_by_limit);

        /* A new recording starts without the flag. */
        assert(measurement_service_keep_async() == ESP_OK);
        run_pending_command();
        assert(measurement_service_start_async() == ESP_OK);
        run_pending_command();
        assert(!snapshot().stopped_by_limit);
        stop_service();
    }

    /* 500 Hz has no limit: an hour of recording is not stopped. */
    start_service();
    measurement_source_rate_hz = 500U;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().record_limit_ms == 0U);
    fake_recorder.recording_ms = 3600000U;
    measurement_service_process_stop_poll();
    measurement_service_enforce_record_limit();
    assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
    assert(!snapshot().stopped_by_limit);
    stop_service();
    measurement_source_rate_hz = 500U;
}


int main(void)
{
    assert(measurement_service_worker_contract_existing_main() == 0);
    test_recording_poll_wait_policy();
    test_autonomous_recorder_error_converges_after_finalization();
    test_successful_stop_refresh_remains_single();
    test_recording_stops_itself_at_the_rate_limit();
    puts("MEASUREMENT_AUTONOMOUS_ERROR_HOST_PASS");
    return 0;
}
