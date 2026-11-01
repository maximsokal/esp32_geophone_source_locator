#include "ui_perf_policy.h"

#include <string.h>


static ui_perf_stage_metrics_t s_stage_metrics;


bool ui_perf_policy_should_log(
    ui_perf_policy_state_t *state,
    uint32_t now_ms,
    uint64_t frames_rendered,
    uint64_t elapsed_us,
    uint32_t target_fps,
    uint64_t max_render_us
)
{
    bool low_fps;
    bool over_frame_budget;
    bool heartbeat_due;
    bool render_warning;
    bool starvation_window;
    bool should_log;

    low_fps = false;
    if (target_fps > 0U) {
        if (elapsed_us == 0U) {
            low_fps = true;
        } else {
            low_fps =
                frames_rendered * 1000000ULL * 100ULL <
                (uint64_t)target_fps * 80ULL * elapsed_us;
        }
    }

    /* One frame budget is 1,000,000 microseconds divided by target FPS. */
    over_frame_budget = target_fps > 0U &&
        max_render_us > (1000000U / target_fps);
    render_warning = ui_perf_policy_render_warning(max_render_us);
    starvation_window = ui_perf_policy_starvation_window(max_render_us);
    if (starvation_window && state->starvation_windows < UINT32_MAX) {
        ++state->starvation_windows;
    }
    heartbeat_due = state->has_logged &&
        (uint32_t)(now_ms - state->last_log_ms) >=
            UI_PERF_LOG_HEARTBEAT_MS;
    should_log =
        !state->has_logged || low_fps || over_frame_budget ||
        render_warning || heartbeat_due;
    if (!should_log) {
        return false;
    }

    state->has_logged = true;
    state->last_log_ms = now_ms;
    state->last_frames_rendered = frames_rendered;
    state->last_elapsed_us = elapsed_us;
    state->last_max_render_us = max_render_us;
    state->last_target_fps = target_fps;
    return true;
}


bool ui_perf_policy_measurements_load_active(
    bool measurements_screen_active,
    bool acquisition_running
)
{
    return measurements_screen_active && acquisition_running;
}


bool ui_perf_policy_live_charts_screen(
    bool measurements_screen,
    bool workflow_screen,
    bool workflow_recording
)
{
    return measurements_screen || (workflow_screen && workflow_recording);
}


uint32_t ui_perf_policy_target_fps(bool load_limited)
{
    return load_limited
        ? UI_PERF_MEASUREMENTS_TARGET_FPS
        : UI_PERF_DEFAULT_TARGET_FPS;
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
    return !load_limited ||
        elapsed_us >= (uint64_t)UI_PERF_CHART_REFRESH_INTERVAL_MS * 1000U;
}


size_t ui_perf_policy_preview_frame_limit(size_t requested_frames, bool load_limited)
{
    return load_limited && requested_frames > UI_PERF_PREVIEW_MAX_FRAMES
        ? UI_PERF_PREVIEW_MAX_FRAMES
        : requested_frames;
}


size_t ui_perf_policy_preview_chunk_limit(size_t requested_frames, bool load_limited)
{
    const size_t chunk_limit = load_limited
        ? UI_PERF_LIMITED_PREVIEW_CHUNK_FRAMES
        : UI_PERF_PREVIEW_CHUNK_FRAMES;
    return requested_frames < chunk_limit ? requested_frames : chunk_limit;
}


bool ui_perf_policy_data_prep_budget_exhausted(uint64_t elapsed_us)
{
    return elapsed_us >= UI_PERF_DATA_PREP_BUDGET_US;
}


bool ui_perf_policy_render_warning(uint64_t max_render_us)
{
    return max_render_us > UI_PERF_RENDER_WARNING_US;
}


bool ui_perf_policy_starvation_window(uint64_t max_render_us)
{
    return max_render_us > UI_PERF_STARVATION_BUDGET_US;
}


uint64_t ui_perf_policy_flush_wire_time_us(
    uint32_t width,
    uint32_t rows,
    uint32_t bytes_per_pixel,
    uint32_t spi_clock_hz
)
{
    if (width == 0U || rows == 0U || bytes_per_pixel == 0U || spi_clock_hz == 0U) {
        return UINT64_MAX;
    }

    const uint64_t bits =
        (uint64_t)width * rows * bytes_per_pixel * 8U;
    return (bits * 1000000U + spi_clock_hz - 1U) / spi_clock_hz;
}


uint32_t ui_perf_policy_flush_chunk_count(uint32_t total_rows, uint32_t rows_per_chunk)
{
    if (rows_per_chunk == 0U) return 0U;
    return (total_rows + rows_per_chunk - 1U) / rows_per_chunk;
}


uint32_t ui_perf_policy_flush_yield_count(uint32_t flush_chunks)
{
    return flush_chunks > 0U ? flush_chunks - 1U : 0U;
}


static void ui_perf_policy_record_stage(
    uint64_t elapsed_us,
    uint64_t *total_us,
    uint64_t *max_us,
    uint64_t *samples
)
{
    *total_us += elapsed_us;
    if (elapsed_us > *max_us) *max_us = elapsed_us;
    ++*samples;
}


void ui_perf_policy_record_update(uint64_t elapsed_us)
{
    ui_perf_policy_record_stage(
        elapsed_us,
        &s_stage_metrics.update_total_us,
        &s_stage_metrics.update_max_us,
        &s_stage_metrics.update_samples
    );
}


void ui_perf_policy_record_render(uint64_t elapsed_us)
{
    ui_perf_policy_record_stage(
        elapsed_us,
        &s_stage_metrics.render_total_us,
        &s_stage_metrics.render_max_us,
        &s_stage_metrics.render_samples
    );
}


void ui_perf_policy_record_flush_submit(uint64_t elapsed_us)
{
    s_stage_metrics.flush_submit_total_us += elapsed_us;
    if (elapsed_us > s_stage_metrics.flush_submit_max_us) {
        s_stage_metrics.flush_submit_max_us = elapsed_us;
    }
    if (s_stage_metrics.flush_chunks < UINT32_MAX) {
        ++s_stage_metrics.flush_chunks;
    }
    if (
        elapsed_us > UI_PERF_FLUSH_CHUNK_BUDGET_US &&
        s_stage_metrics.flush_submit_over_budget_chunks < UINT32_MAX
    ) {
        ++s_stage_metrics.flush_submit_over_budget_chunks;
    }
}


void ui_perf_policy_record_flush_wait(uint64_t elapsed_us, bool yielded)
{
    s_stage_metrics.flush_wait_total_us += elapsed_us;
    if (elapsed_us > s_stage_metrics.flush_wait_max_us) {
        s_stage_metrics.flush_wait_max_us = elapsed_us;
    }
    if (s_stage_metrics.flush_waits < UINT32_MAX) {
        ++s_stage_metrics.flush_waits;
    }
    if (yielded && s_stage_metrics.flush_yields < UINT32_MAX) {
        ++s_stage_metrics.flush_yields;
    }
}


void ui_perf_policy_record_data_prep(uint64_t elapsed_us)
{
    ui_perf_policy_record_stage(
        elapsed_us,
        &s_stage_metrics.data_prep_total_us,
        &s_stage_metrics.data_prep_max_us,
        &s_stage_metrics.data_prep_samples
    );
}


ui_perf_stage_metrics_t ui_perf_policy_stage_snapshot(void)
{
    return s_stage_metrics;
}


ui_perf_stage_metrics_t ui_perf_policy_stage_take(void)
{
    const ui_perf_stage_metrics_t snapshot = s_stage_metrics;
    memset(&s_stage_metrics, 0, sizeof(s_stage_metrics));
    return snapshot;
}
