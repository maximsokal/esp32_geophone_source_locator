#include "ui_perf_policy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>


#if defined(UI_PERF_POLICY_TEST_BASELINE)
static ui_perf_stage_metrics_t s_baseline_stage_metrics;

bool ui_perf_policy_measurements_load_active(bool measurements_screen_active, bool acquisition_running)
{
    (void)measurements_screen_active;
    (void)acquisition_running;
    return false;
}

bool ui_perf_policy_live_charts_screen(bool measurements_screen, bool workflow_screen, bool workflow_recording)
{
    (void)workflow_screen;
    (void)workflow_recording;
    return measurements_screen;
}

uint32_t ui_perf_policy_target_fps(bool load_limited)
{
    (void)load_limited;
    return UI_PERF_DEFAULT_TARGET_FPS;
}

uint32_t ui_perf_policy_update_interval_ms(bool load_limited)
{
    const uint32_t fps = ui_perf_policy_target_fps(load_limited);
    return fps > 0U ? (1000U + fps - 1U) / fps : 0U;
}

uint64_t ui_perf_policy_update_interval_us(bool load_limited)
{
    const uint32_t fps = ui_perf_policy_target_fps(load_limited);
    return fps > 0U ? 1000000U / fps : 0U;
}

bool ui_perf_policy_chart_refresh_due(uint64_t elapsed_us, bool load_limited)
{
    (void)elapsed_us;
    return !load_limited;
}

size_t ui_perf_policy_preview_frame_limit(size_t requested_frames, bool load_limited)
{
    (void)load_limited;
    return requested_frames;
}

size_t ui_perf_policy_preview_chunk_limit(size_t requested_frames, bool load_limited)
{
    (void)load_limited;
    return requested_frames;
}

bool ui_perf_policy_data_prep_budget_exhausted(uint64_t elapsed_us)
{
    (void)elapsed_us;
    return false;
}

bool ui_perf_policy_render_warning(uint64_t max_render_us)
{
    (void)max_render_us;
    return false;
}

bool ui_perf_policy_starvation_window(uint64_t max_render_us)
{
    (void)max_render_us;
    return false;
}

uint64_t ui_perf_policy_flush_wire_time_us(
    uint32_t width,
    uint32_t rows,
    uint32_t bytes_per_pixel,
    uint32_t spi_clock_hz
)
{
    if (spi_clock_hz == 0U) return UINT64_MAX;
    const uint64_t bits = (uint64_t)width * rows * bytes_per_pixel * 8U;
    return (bits * 1000000U + spi_clock_hz - 1U) / spi_clock_hz;
}

uint32_t ui_perf_policy_flush_chunk_count(uint32_t total_rows, uint32_t rows_per_chunk)
{
    return rows_per_chunk == 0U ? 0U : (total_rows + rows_per_chunk - 1U) / rows_per_chunk;
}

uint32_t ui_perf_policy_flush_yield_count(uint32_t flush_chunks)
{
    (void)flush_chunks;
    return 0U;
}

void ui_perf_policy_record_update(uint64_t elapsed_us) { (void)elapsed_us; }
void ui_perf_policy_record_render(uint64_t elapsed_us) { (void)elapsed_us; }

void ui_perf_policy_record_flush_submit(uint64_t elapsed_us)
{
    s_baseline_stage_metrics.flush_submit_total_us += elapsed_us;
    if (elapsed_us > s_baseline_stage_metrics.flush_submit_max_us) {
        s_baseline_stage_metrics.flush_submit_max_us = elapsed_us;
    }
    ++s_baseline_stage_metrics.flush_chunks;
}

void ui_perf_policy_record_flush_wait(uint64_t elapsed_us, bool yielded)
{
    (void)elapsed_us;
    (void)yielded;
}

void ui_perf_policy_record_data_prep(uint64_t elapsed_us) { (void)elapsed_us; }

ui_perf_stage_metrics_t ui_perf_policy_stage_snapshot(void)
{
    return s_baseline_stage_metrics;
}

ui_perf_stage_metrics_t ui_perf_policy_stage_take(void)
{
    const ui_perf_stage_metrics_t result = s_baseline_stage_metrics;
    memset(&s_baseline_stage_metrics, 0, sizeof(s_baseline_stage_metrics));
    return result;
}
#endif


static void test_first_healthy_window_and_heartbeat(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 0U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 10000U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 59999U, 240U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 60000U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 119999U, 240U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 120000U, 240U, 10000000U, 24U, 1000U));
}


static void test_fps_threshold_is_strictly_below_eighty_percent(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 10U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 10010U, 192U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 20010U, 191U, 10000000U, 24U, 1000U));
    /* Exactly 80% right after a logged low window is healthy again: no log. */
    assert(!ui_perf_policy_should_log(&state, 30010U, 192U, 10000000U, 24U, 1000U));
}


static void test_render_budget_boundary(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 0U, 240U, 10000000U, 24U, 41666U));
    assert(!ui_perf_policy_should_log(&state, 10000U, 240U, 10000000U, 24U, 41666U));
    assert(ui_perf_policy_should_log(&state, 20000U, 240U, 10000000U, 24U, 41667U));
}


static void test_heartbeat_handles_uint32_wraparound(void)
{
    ui_perf_policy_state_t state = {0};
    const uint32_t start_ms = UINT32_MAX - 1000U;

    assert(ui_perf_policy_should_log(
        &state, start_ms, 240U, 10000000U, 24U, 1000U
    ));
    assert(!ui_perf_policy_should_log(
        &state, (uint32_t)(start_ms + 59999U), 240U, 10000000U, 24U, 1000U
    ));
    assert(ui_perf_policy_should_log(
        &state, (uint32_t)(start_ms + 60000U), 240U, 10000000U, 24U, 1000U
    ));
}


static void test_U1b_workflow_recording_gets_the_same_load_profile_as_measurements(void)
{
    /* The workflow's RECORDING step draws the same live charts, so it must be throttled like MEASUREMENTS. */
    assert(ui_perf_policy_live_charts_screen(true, false, false));
    assert(ui_perf_policy_live_charts_screen(false, true, true));
    assert(!ui_perf_policy_live_charts_screen(false, true, false));    /* other workflow steps stay at full rate */
    assert(!ui_perf_policy_live_charts_screen(false, false, true));    /* a stale flag on another screen counts for nothing */
    assert(!ui_perf_policy_live_charts_screen(false, false, false));
    const bool recording_in_workflow = ui_perf_policy_measurements_load_active(
        ui_perf_policy_live_charts_screen(false, true, true), true);
    assert(recording_in_workflow);
    assert(ui_perf_policy_target_fps(recording_in_workflow) == UI_PERF_MEASUREMENTS_TARGET_FPS);
    assert(!ui_perf_policy_measurements_load_active(ui_perf_policy_live_charts_screen(false, true, true), false));
}


static void test_U1_measurement_load_policy_throttles_live_and_recording(void)
{
    /* LIVE and RECORDING both keep seismic_acquisition_snapshot.running true. */
    const bool live_preview_load = ui_perf_policy_measurements_load_active(true, true);
    const bool recording_load = ui_perf_policy_measurements_load_active(true, true);
    const bool other_screen_load = ui_perf_policy_measurements_load_active(false, true);
    const bool stopped_acquisition_load = ui_perf_policy_measurements_load_active(true, false);

    assert(live_preview_load);
    assert(recording_load);
    assert(!other_screen_load);
    assert(!stopped_acquisition_load);
    assert(UI_PERF_DEFAULT_TARGET_FPS == 24U);
    assert(UI_PERF_MEASUREMENTS_TARGET_FPS == 4U);
    assert(ui_perf_policy_target_fps(live_preview_load) == UI_PERF_MEASUREMENTS_TARGET_FPS);
    assert(ui_perf_policy_target_fps(recording_load) == UI_PERF_MEASUREMENTS_TARGET_FPS);
    assert(ui_perf_policy_target_fps(other_screen_load) == UI_PERF_DEFAULT_TARGET_FPS);
    assert(ui_perf_policy_target_fps(stopped_acquisition_load) == UI_PERF_DEFAULT_TARGET_FPS);
    assert(ui_perf_policy_update_interval_ms(true) == 250U);
    assert(ui_perf_policy_update_interval_ms(false) == 42U);
    assert(ui_perf_policy_update_interval_us(true) == 250000U);
    assert(ui_perf_policy_update_interval_us(false) == 41666U);
    assert(UI_PERF_PREVIEW_MAX_FRAMES == 4096U);
    assert(ui_perf_policy_preview_frame_limit(40960U, true) == UI_PERF_PREVIEW_MAX_FRAMES);
    assert(ui_perf_policy_preview_frame_limit(40960U, false) == 40960U);
    assert(UI_PERF_DATA_PREP_BUDGET_US == 50000U);
    assert(!ui_perf_policy_data_prep_budget_exhausted(49999U));
    assert(ui_perf_policy_data_prep_budget_exhausted(50000U));
    assert(ui_perf_policy_data_prep_budget_exhausted(50001U));

    uint64_t fake_elapsed_us = 0U;
    uint32_t work_chunks = 0U;
    while (!ui_perf_policy_data_prep_budget_exhausted(fake_elapsed_us)) {
        fake_elapsed_us += 5000U;
        ++work_chunks;
    }
    assert(work_chunks == 10U);
    assert(fake_elapsed_us == UI_PERF_DATA_PREP_BUDGET_US);
    assert(ui_perf_policy_data_prep_budget_exhausted(fake_elapsed_us));
}


static void test_U2_flush_chunks_stay_under_5ms_and_yield_between(void)
{
    const uint64_t chunk_wire_us = ui_perf_policy_flush_wire_time_us(
        320U, UI_PERF_FLUSH_CHUNK_ROWS, 2U, 40000000U
    );
    const uint32_t chunks =
        ui_perf_policy_flush_chunk_count(240U, UI_PERF_FLUSH_CHUNK_ROWS);
    const uint32_t expected_yields = ui_perf_policy_flush_yield_count(chunks);
    assert(UI_PERF_FLUSH_CHUNK_ROWS == 8U);
    assert(UI_PERF_FLUSH_CHUNK_BUDGET_US == 5000U);
    assert(chunk_wire_us == 1024U);
    assert(chunk_wire_us <= UI_PERF_FLUSH_CHUNK_BUDGET_US);
    assert(ui_perf_policy_flush_wire_time_us(320U, 39U, 2U, 40000000U) == 4992U);
    assert(ui_perf_policy_flush_wire_time_us(320U, 40U, 2U, 40000000U) >
        UI_PERF_FLUSH_CHUNK_BUDGET_US);
    assert(ui_perf_policy_flush_chunk_count(
        UI_PERF_FLUSH_CHUNK_ROWS, UI_PERF_FLUSH_CHUNK_ROWS
    ) == 1U);
    assert(ui_perf_policy_flush_chunk_count(
        UI_PERF_FLUSH_CHUNK_ROWS + 1U, UI_PERF_FLUSH_CHUNK_ROWS
    ) == 2U);
    assert(ui_perf_policy_flush_yield_count(1U) == 0U);
    assert(ui_perf_policy_flush_yield_count(2U) == 1U);
    assert(chunks == 30U);
    assert(expected_yields == 29U);

    (void)ui_perf_policy_stage_take();
    for (uint32_t chunk = 0U; chunk < chunks; ++chunk) {
        ui_perf_policy_record_flush_submit(chunk_wire_us);
        if (chunk < expected_yields) {
            ui_perf_policy_record_flush_wait(10000U, true);
        }
    }
    const ui_perf_stage_metrics_t metrics = ui_perf_policy_stage_take();
    assert(metrics.flush_chunks == chunks);
    assert(metrics.flush_yields == expected_yields);
    assert(metrics.flush_waits == expected_yields);
    assert(metrics.flush_submit_max_us == 1024U);
    assert(metrics.flush_wait_max_us == 10000U);
    assert(metrics.flush_submit_over_budget_chunks == 0U);
}


static void test_U3_render_warning_boundary_is_strictly_over_50ms(void)
{
    assert(!ui_perf_policy_render_warning(49000U));
    assert(!ui_perf_policy_render_warning(50000U));
    assert(ui_perf_policy_render_warning(51000U));

    ui_perf_policy_state_t state = {
        .has_logged = true,
        .last_log_ms = 0U,
    };
    assert(!ui_perf_policy_should_log(&state, 1000U, 4U, 1000000U, 0U, 49000U));
    assert(!ui_perf_policy_should_log(&state, 2000U, 4U, 1000000U, 0U, 50000U));
    assert(ui_perf_policy_should_log(&state, 3000U, 4U, 1000000U, 0U, 51000U));
}


static void test_U4_old_frame_starves_150ms_budget_and_recording_cap_does_not(void)
{
    ui_perf_policy_state_t old_state = {0};
    assert(UI_PERF_STARVATION_BUDGET_US == 150000U);
    assert(!ui_perf_policy_starvation_window(149999U));
    assert(!ui_perf_policy_starvation_window(150000U));
    assert(ui_perf_policy_starvation_window(150001U));
    assert(ui_perf_policy_starvation_window(233000U));
    assert(ui_perf_policy_should_log(&old_state, 10000U, 4U, 10000000U, 24U, 233000U));
    assert(old_state.starvation_windows == 1U);

    ui_perf_policy_state_t capped_state = {0};
    const uint64_t capped_frame_us = UI_PERF_DATA_PREP_BUDGET_US;
    assert(!ui_perf_policy_starvation_window(capped_frame_us));
    assert(ui_perf_policy_should_log(&capped_state, 10000U, 40U, 10000000U, 4U, capped_frame_us));
    assert(capped_state.starvation_windows == 0U);
}


static void test_U5_chart_refresh_interval_boundary(void)
{
    assert(UI_PERF_CHART_REFRESH_INTERVAL_MS == 125U);
    assert(!ui_perf_policy_chart_refresh_due(124999U, true));
    assert(ui_perf_policy_chart_refresh_due(125000U, true));
    assert(ui_perf_policy_chart_refresh_due(125001U, true));
    assert(ui_perf_policy_chart_refresh_due(0U, false));
}


static void test_U6_preview_caps_and_chunk_size_boundaries(void)
{
    assert(UI_PERF_PREVIEW_MAX_FRAMES == 4096U);
    assert(ui_perf_policy_preview_frame_limit(4095U, true) == 4095U);
    assert(ui_perf_policy_preview_frame_limit(4096U, true) == 4096U);
    assert(ui_perf_policy_preview_frame_limit(4097U, true) == 4096U);

    assert(UI_PERF_LIMITED_PREVIEW_CHUNK_FRAMES == 128U);
    assert(ui_perf_policy_preview_chunk_limit(127U, true) == 127U);
    assert(ui_perf_policy_preview_chunk_limit(128U, true) == 128U);
    assert(ui_perf_policy_preview_chunk_limit(129U, true) == 128U);

    assert(UI_PERF_PREVIEW_CHUNK_FRAMES == 1024U);
    assert(ui_perf_policy_preview_chunk_limit(1023U, false) == 1023U);
    assert(ui_perf_policy_preview_chunk_limit(1024U, false) == 1024U);
    assert(ui_perf_policy_preview_chunk_limit(1025U, false) == 1024U);
}


static bool run_requested_test(const char *name)
{
    if (strcmp(name, "U1") == 0) test_U1_measurement_load_policy_throttles_live_and_recording();
    if (strcmp(name, "U1b") == 0) test_U1b_workflow_recording_gets_the_same_load_profile_as_measurements();
    else if (strcmp(name, "U2") == 0) test_U2_flush_chunks_stay_under_5ms_and_yield_between();
    else if (strcmp(name, "U3") == 0) test_U3_render_warning_boundary_is_strictly_over_50ms();
    else if (strcmp(name, "U4") == 0) test_U4_old_frame_starves_150ms_budget_and_recording_cap_does_not();
    else if (strcmp(name, "U5") == 0) test_U5_chart_refresh_interval_boundary();
    else if (strcmp(name, "U6") == 0) test_U6_preview_caps_and_chunk_size_boundaries();
    else return false;
    return true;
}


int main(int argc, char **argv)
{
    if (argc == 2) return run_requested_test(argv[1]) ? 0 : 2;

    test_first_healthy_window_and_heartbeat();
    test_fps_threshold_is_strictly_below_eighty_percent();
    test_render_budget_boundary();
    test_heartbeat_handles_uint32_wraparound();
    test_U1_measurement_load_policy_throttles_live_and_recording();
    test_U1b_workflow_recording_gets_the_same_load_profile_as_measurements();
    test_U2_flush_chunks_stay_under_5ms_and_yield_between();
    test_U3_render_warning_boundary_is_strictly_over_50ms();
    test_U4_old_frame_starves_150ms_budget_and_recording_cap_does_not();
    test_U5_chart_refresh_interval_boundary();
    test_U6_preview_caps_and_chunk_size_boundaries();
    return 0;
}
