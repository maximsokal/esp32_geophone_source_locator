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

uint32_t ui_perf_policy_target_fps(bool remote_client)
{
    (void)remote_client;
    return 24U;
}

uint32_t ui_perf_policy_update_interval_ms(bool remote_client)
{
    const uint32_t fps = ui_perf_policy_target_fps(remote_client);
    return fps > 0U ? (1000U + fps - 1U) / fps : 0U;
}

uint64_t ui_perf_policy_update_interval_us(bool remote_client)
{
    const uint32_t fps = ui_perf_policy_target_fps(remote_client);
    return fps > 0U ? 1000000U / fps : 0U;
}

uint64_t ui_perf_policy_update_gap_us(bool remote_client, bool load_limited, uint64_t last_render_us)
{
    (void)load_limited;
    (void)last_render_us;
    return ui_perf_policy_update_interval_us(remote_client);
}

bool ui_perf_policy_remote_client_active(bool seen, uint32_t last_request_ms, uint32_t now_ms)
{
    (void)seen;
    (void)last_request_ms;
    (void)now_ms;
    return false;
}

void ui_perf_pacer_init(ui_perf_pacer_t *pacer, uint64_t now_us) { (void)pacer; (void)now_us; }

ui_perf_pacer_step_t ui_perf_pacer_begin(ui_perf_pacer_t *pacer, uint64_t now_us, bool storage_pending)
{
    (void)pacer;
    (void)now_us;
    (void)storage_pending;
    return UI_PERF_PACER_RUN;
}

bool ui_perf_pacer_periodic_due(const ui_perf_pacer_t *pacer, uint64_t now_us)
{
    (void)pacer;
    (void)now_us;
    return true;
}

void ui_perf_pacer_frame_done(
    ui_perf_pacer_t *pacer, uint64_t now_us, bool periodic_ran, bool remote_client, bool load_limited,
    uint64_t last_render_us)
{
    (void)pacer;
    (void)now_us;
    (void)periodic_ran;
    (void)remote_client;
    (void)load_limited;
    (void)last_render_us;
}

void ui_perf_pacer_periodic_now(ui_perf_pacer_t *pacer, uint64_t now_us) { (void)pacer; (void)now_us; }

uint64_t ui_perf_policy_last_render_us(void)
{
    return 0U;
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
    /* The workflow's RECORDING step is throttled by the same 4 FPS ceiling as every other screen. */
    assert(ui_perf_policy_target_fps(false) == UI_PERF_TARGET_FPS);
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
    /* The screen never redraws more than 4 times a second, whatever it shows; with a web client it is 1. */
    assert(UI_PERF_TARGET_FPS == 4U);
    assert(UI_PERF_REMOTE_CLIENT_TARGET_FPS == 1U);
    assert(ui_perf_policy_target_fps(false) == 4U);
    assert(ui_perf_policy_target_fps(true) == 1U);
    assert(ui_perf_policy_update_interval_ms(false) == 250U);
    assert(ui_perf_policy_update_interval_ms(true) == 1000U);
    assert(ui_perf_policy_update_interval_us(false) == 250000U);
    assert(ui_perf_policy_update_interval_us(true) == 1000000U);
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


/*
 * Bench log 2026-11-01 21:46: a frame of the live charts costs ~340 ms of core 0 and the 250 ms target interval
 * left no rest: taskLVGL held 66 % of the core, IDLE0 3 %, the watchdog fired and nrf_topology (priority below
 * LVGL's) did not run for up to 58 s. After a costly frame the task now rests twice its cost.
 */
static void test_U7_a_costly_live_frame_is_followed_by_a_rest_that_leaves_core_time_to_others(void)
{
    /* Cheap frames keep the 4 FPS interval. */
    assert(ui_perf_policy_update_gap_us(false, true, 0U) == 250000U);
    assert(ui_perf_policy_update_gap_us(false, true, 11000U) == 250000U);
    assert(ui_perf_policy_update_gap_us(false, true, 125000U) == 250000U);      /* 2 x 125 ms = the interval itself */
    /* Costly frames rest twice their cost: LVGL then holds a third of the core, not two thirds. */
    assert(ui_perf_policy_update_gap_us(false, true, 126000U) == 252000U);
    assert(ui_perf_policy_update_gap_us(false, true, 340000U) == 680000U);
    assert(ui_perf_policy_update_gap_us(false, true, 345012U) == 690024U);
    /* The rest is bounded so that the picture still moves. */
    assert(ui_perf_policy_update_gap_us(false, true, 400000U) == UI_PERF_LOAD_REST_MAX_US);
    assert(ui_perf_policy_update_gap_us(false, true, 5000000U) == UI_PERF_LOAD_REST_MAX_US);
    /* The share of the core LVGL keeps stays at or under a third while the rest is not capped. */
    for (uint64_t render_us = 130000U; render_us <= 400000U; render_us += 10000U) {
        const uint64_t period_us = render_us + ui_perf_policy_update_gap_us(false, true, render_us);
        assert(render_us * 3U <= period_us + 3U);
    }
    /* Other screens are not throttled by their frame cost: just the 4 FPS interval. */
    assert(ui_perf_policy_update_gap_us(false, false, 340000U) == 250000U);
    assert(ui_perf_policy_update_gap_us(false, false, 0U) == 250000U);
    /* With a web client the interval is a second; a frame rest that is shorter does not shorten it. */
    assert(ui_perf_policy_update_gap_us(true, false, 340000U) == 1000000U);
    assert(ui_perf_policy_update_gap_us(true, true, 340000U) == 1000000U);
    assert(ui_perf_policy_update_gap_us(true, true, 5000000U) == 1000000U);
}

static void test_U7b_the_last_render_cost_is_the_one_recorded(void)
{
    ui_perf_policy_record_render(11000U);
    assert(ui_perf_policy_last_render_us() == 11000U);
    ui_perf_policy_record_render(345012U);
    assert(ui_perf_policy_last_render_us() == 345012U);
    ui_perf_policy_record_render(9000U);                 /* the latest, not the largest */
    assert(ui_perf_policy_last_render_us() == 9000U);
    (void)ui_perf_policy_stage_take();                   /* the log window reset leaves it */
    assert(ui_perf_policy_last_render_us() == 9000U);
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


/*
 * Bench log 2026-11-01 21:46: a chunk of the live charts cost ~20 ms and nothing in the log said whether that was a
 * few big chunks or many small ones, nor how much of it was per pixel. The profile of a window tells.
 */
static void test_U8_chunk_profile_splits_a_window_into_chunks_pixels_and_draw_time(void)
{
    (void)ui_perf_policy_stage_take();
    ui_perf_profile_t empty = ui_perf_policy_profile(&(ui_perf_stage_metrics_t){0});
    assert(empty.chunks_per_frame_x10 == 0U && empty.chunk_draw_avg_us == 0U && empty.draw_ns_per_pixel == 0U);
    assert(ui_perf_policy_profile(NULL).pixels_per_chunk == 0U);

    /* frame 1: five strips of 58x8 px at 2 ms; frame 2: twelve bands of 300x8 px at 14 ms */
    for (unsigned i = 0U; i < 5U; ++i) ui_perf_policy_record_chunk_draw(2000U, 58U * 8U);
    ui_perf_policy_record_frame_chunks(5U);
    ui_perf_policy_record_render(10000U);
    for (unsigned i = 0U; i < 12U; ++i) ui_perf_policy_record_chunk_draw(14000U, 300U * 8U);
    ui_perf_policy_record_frame_chunks(12U);
    ui_perf_policy_record_render(168000U);

    const ui_perf_stage_metrics_t window = ui_perf_policy_stage_take();
    const ui_perf_profile_t profile = ui_perf_policy_profile(&window);
    assert(window.chunk_samples == 17U);
    assert(profile.chunks_per_frame_x10 == 85U);                 /* 17 chunks over 2 frames */
    assert(profile.chunks_per_frame_max == 12U);
    assert(profile.chunk_draw_avg_us == 10470U);                 /* 178000 us over 17 chunks */
    assert(profile.chunk_draw_max_us == 14000U);
    assert(profile.pixels_per_chunk == 1830U);                   /* 31120 px over 17 chunks */
    assert(profile.draw_ns_per_pixel == 5719U);                  /* 178000 us over 31120 px */

    const ui_perf_stage_metrics_t after = ui_perf_policy_stage_take();   /* the window reset clears it */
    assert(after.chunk_samples == 0U && after.frame_chunks_max == 0U && after.chunk_pixels_total == 0U);
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


/* The ceiling: a frame never starts less than the 4 FPS interval after the previous one, a key press included. */
static void test_P1_no_frame_comes_sooner_than_the_four_fps_interval(void)
{
    ui_perf_pacer_t pacer;
    ui_perf_pacer_init(&pacer, 1000000U);
    assert(ui_perf_pacer_begin(&pacer, 1000000U, false) == UI_PERF_PACER_RUN);        /* the first one is at once */
    assert(ui_perf_pacer_periodic_due(&pacer, 1000000U));                              /* and so is the first refresh */
    ui_perf_pacer_frame_done(&pacer, 1000000U, true, false, false, 10000U);
    assert(ui_perf_pacer_begin(&pacer, 1100000U, false) == UI_PERF_PACER_WAIT);
    assert(ui_perf_pacer_begin(&pacer, 1239999U, false) == UI_PERF_PACER_WAIT);       /* 250 ms less one 10 ms LVGL tick */
    assert(ui_perf_pacer_begin(&pacer, 1240000U, false) == UI_PERF_PACER_RUN);
    assert(ui_perf_pacer_begin(&pacer, 1250000U, false) == UI_PERF_PACER_RUN);
    /* Something asks for a frame at every 5 ms tick for 10 s: they still come no closer than 240 ms. */
    ui_perf_pacer_frame_done(&pacer, 1250000U, false, false, false, 0U);
    uint32_t frames = 0U;
    uint64_t previous_us = 1250000U;
    uint64_t closest_us = UINT64_MAX;
    for (uint64_t now = 1255000U; now < 11250000U; now += 5000U) {
        if (ui_perf_pacer_begin(&pacer, now, false) == UI_PERF_PACER_RUN) {
            if (now - previous_us < closest_us) closest_us = now - previous_us;
            previous_us = now;
            ui_perf_pacer_frame_done(&pacer, now, false, false, false, 0U);
            ++frames;
        }
    }
    assert(closest_us >= 240000U);                                                   /* the ceiling */
    assert(frames <= 42U);                                                           /* 4.2 FPS at the very most */
    assert(frames >= 40U);                                                           /* and it does keep drawing */
    assert(ui_perf_pacer_begin(NULL, 0U, false) == UI_PERF_PACER_WAIT);
}


/* A periodic refresh waits for its own interval (a second with a web client); input only needs the ceiling. */
static void test_P2_periodic_refresh_follows_the_client_dependent_interval(void)
{
    ui_perf_pacer_t pacer;
    ui_perf_pacer_init(&pacer, 0U);
    ui_perf_pacer_frame_done(&pacer, 0U, true, false, false, 0U);
    assert(!ui_perf_pacer_periodic_due(&pacer, 100000U));
    assert(!ui_perf_pacer_periodic_due(&pacer, 239999U));
    assert(ui_perf_pacer_periodic_due(&pacer, 240000U));

    ui_perf_pacer_frame_done(&pacer, 1000000U, true, true, false, 0U);                /* a client is connected */
    assert(!ui_perf_pacer_periodic_due(&pacer, 1250000U));
    assert(!ui_perf_pacer_periodic_due(&pacer, 1989999U));
    assert(ui_perf_pacer_periodic_due(&pacer, 1990000U));

    /* A key press inside that second draws a frame at the next tick, and leaves the periodic due time alone. */
    assert(ui_perf_pacer_begin(&pacer, 1250000U, false) == UI_PERF_PACER_RUN);
    ui_perf_pacer_frame_done(&pacer, 1250000U, false, true, false, 0U);
    assert(!ui_perf_pacer_periodic_due(&pacer, 1500000U));
    assert(ui_perf_pacer_periodic_due(&pacer, 1990000U));
    assert(ui_perf_pacer_begin(&pacer, 1300000U, false) == UI_PERF_PACER_WAIT);      /* key presses are capped too */

    /* A costly live-chart frame rests twice its cost, which a connected client's second does not shorten. */
    ui_perf_pacer_frame_done(&pacer, 2000000U, true, false, true, 340000U);
    assert(!ui_perf_pacer_periodic_due(&pacer, 2669999U));
    assert(ui_perf_pacer_periodic_due(&pacer, 2670000U));

    /* A live-chart screen that has just opened draws its first picture without waiting out the old interval. */
    ui_perf_pacer_frame_done(&pacer, 3000000U, true, true, false, 0U);
    ui_perf_pacer_periodic_now(&pacer, 3100000U);
    assert(ui_perf_pacer_periodic_due(&pacer, 3100000U));
    assert(ui_perf_pacer_begin(&pacer, 3100000U, false) == UI_PERF_PACER_WAIT);      /* the ceiling still applies */
    assert(!ui_perf_pacer_periodic_due(NULL, 0U));
}


/* No frame starts while an SD operation is queued, for two ticks at most: a recording keeps the card busy. */
static void test_P3_a_frame_waits_for_queued_sd_operations_but_not_for_ever(void)
{
    ui_perf_pacer_t pacer;
    ui_perf_pacer_init(&pacer, 0U);
    assert(UI_PERF_STORAGE_DEFER_MAX_US == 500000U);
    assert(ui_perf_pacer_begin(&pacer, 0U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&pacer, 250000U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&pacer, 499999U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&pacer, 500000U, true) == UI_PERF_PACER_RUN);         /* forced after two ticks */
    ui_perf_pacer_frame_done(&pacer, 500000U, true, false, false, 0U);

    /* The wait starts again with the next busy tick; it is not carried over from an earlier one. */
    assert(ui_perf_pacer_begin(&pacer, 750000U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&pacer, 1000000U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&pacer, 1250000U, true) == UI_PERF_PACER_RUN);

    /* A card that falls quiet releases the frame at once, and the next busy spell counts from its own start. */
    ui_perf_pacer_t quiet;
    ui_perf_pacer_init(&quiet, 0U);
    assert(ui_perf_pacer_begin(&quiet, 0U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&quiet, 250000U, false) == UI_PERF_PACER_RUN);
    assert(ui_perf_pacer_begin(&quiet, 300000U, true) == UI_PERF_PACER_DEFER);
    assert(ui_perf_pacer_begin(&quiet, 700000U, true) == UI_PERF_PACER_DEFER);       /* 400 ms into this spell */
    assert(ui_perf_pacer_begin(&quiet, 800000U, true) == UI_PERF_PACER_RUN);

    /* The ceiling comes first: a tick inside it is a wait and does not start the busy timer. */
    ui_perf_pacer_t early;
    ui_perf_pacer_init(&early, 0U);
    ui_perf_pacer_frame_done(&early, 0U, true, false, false, 0U);
    assert(ui_perf_pacer_begin(&early, 100000U, true) == UI_PERF_PACER_WAIT);
    assert(!early.deferring);
}


static void test_P4_a_web_client_counts_as_active_for_ten_seconds_after_its_last_request(void)
{
    assert(UI_PERF_REMOTE_ACTIVITY_WINDOW_MS == 10000U);
    assert(!ui_perf_policy_remote_client_active(false, 0U, 5000U));                   /* nobody ever asked */
    assert(ui_perf_policy_remote_client_active(true, 1000U, 1000U));
    assert(ui_perf_policy_remote_client_active(true, 1000U, 10999U));
    assert(!ui_perf_policy_remote_client_active(true, 1000U, 11000U));
    assert(!ui_perf_policy_remote_client_active(true, 1000U, 500000U));
    /* The 32-bit millisecond clock wraps after 49 days: an age is still an age. */
    assert(ui_perf_policy_remote_client_active(true, 0xFFFFFF00U, 0x00000100U));
    assert(!ui_perf_policy_remote_client_active(true, 0xFFFFFF00U, 0x00003000U));
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
    else if (strcmp(name, "U7") == 0) test_U7_a_costly_live_frame_is_followed_by_a_rest_that_leaves_core_time_to_others();
    else if (strcmp(name, "U7b") == 0) test_U7b_the_last_render_cost_is_the_one_recorded();
    else if (strcmp(name, "U8") == 0) test_U8_chunk_profile_splits_a_window_into_chunks_pixels_and_draw_time();
    else if (strcmp(name, "P1") == 0) test_P1_no_frame_comes_sooner_than_the_four_fps_interval();
    else if (strcmp(name, "P2") == 0) test_P2_periodic_refresh_follows_the_client_dependent_interval();
    else if (strcmp(name, "P3") == 0) test_P3_a_frame_waits_for_queued_sd_operations_but_not_for_ever();
    else if (strcmp(name, "P4") == 0) test_P4_a_web_client_counts_as_active_for_ten_seconds_after_its_last_request();
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
    test_U7_a_costly_live_frame_is_followed_by_a_rest_that_leaves_core_time_to_others();
    test_P1_no_frame_comes_sooner_than_the_four_fps_interval();
    test_P2_periodic_refresh_follows_the_client_dependent_interval();
    test_P3_a_frame_waits_for_queued_sd_operations_but_not_for_ever();
    test_P4_a_web_client_counts_as_active_for_ten_seconds_after_its_last_request();
    test_U7b_the_last_render_cost_is_the_one_recorded();
    test_U8_chunk_profile_splits_a_window_into_chunks_pixels_and_draw_time();
    return 0;
}
