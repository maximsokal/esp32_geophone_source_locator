#include "ui_perf_policy.h"

#include <string.h>


static ui_perf_stage_metrics_t s_stage_metrics;
static uint64_t s_last_render_us;


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


uint32_t ui_perf_policy_target_fps(bool remote_client)
{
    return remote_client
        ? UI_PERF_REMOTE_CLIENT_TARGET_FPS
        : UI_PERF_TARGET_FPS;
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
    const uint64_t interval_us = ui_perf_policy_update_interval_us(remote_client);
    if (!load_limited) return interval_us;
    uint64_t rest_us = last_render_us * UI_PERF_LOAD_REST_FACTOR;
    if (rest_us > UI_PERF_LOAD_REST_MAX_US) rest_us = UI_PERF_LOAD_REST_MAX_US;
    return rest_us > interval_us ? rest_us : interval_us;
}


bool ui_perf_policy_remote_client_active(bool seen, uint32_t last_request_ms, uint32_t now_ms)
{
    if (!seen) return false;
    return (uint32_t)(now_ms - last_request_ms) < UI_PERF_REMOTE_ACTIVITY_WINDOW_MS;
}


void ui_perf_pacer_init(ui_perf_pacer_t *pacer, uint64_t now_us)
{
    if (pacer == NULL) return;
    memset(pacer, 0, sizeof(*pacer));
    pacer->floor_due_us = now_us;
    pacer->periodic_due_us = now_us;
}


ui_perf_pacer_step_t ui_perf_pacer_begin(ui_perf_pacer_t *pacer, uint64_t now_us, bool storage_pending)
{
    if (pacer == NULL) return UI_PERF_PACER_WAIT;
    if (now_us + UI_PERF_FRAME_JITTER_US < pacer->floor_due_us) return UI_PERF_PACER_WAIT;

    if (!storage_pending) {
        pacer->deferring = false;
        return UI_PERF_PACER_RUN;
    }
    if (!pacer->deferring) {
        pacer->deferring = true;
        pacer->defer_started_us = now_us;
    }
    return now_us - pacer->defer_started_us < UI_PERF_STORAGE_DEFER_MAX_US
        ? UI_PERF_PACER_DEFER
        : UI_PERF_PACER_RUN;
}


bool ui_perf_pacer_periodic_due(const ui_perf_pacer_t *pacer, uint64_t now_us)
{
    return pacer != NULL && now_us + UI_PERF_FRAME_JITTER_US >= pacer->periodic_due_us;
}


void ui_perf_pacer_frame_done(
    ui_perf_pacer_t *pacer,
    uint64_t now_us,
    bool periodic_ran,
    bool remote_client,
    bool load_limited,
    uint64_t last_render_us
)
{
    if (pacer == NULL) return;
    pacer->deferring = false;
    pacer->floor_due_us = now_us + ui_perf_policy_update_interval_us(false);
    if (periodic_ran) {
        pacer->periodic_due_us = now_us + ui_perf_policy_update_gap_us(remote_client, load_limited, last_render_us);
    }
}


void ui_perf_pacer_periodic_now(ui_perf_pacer_t *pacer, uint64_t now_us)
{
    if (pacer == NULL) return;
    pacer->periodic_due_us = now_us;
}


uint64_t ui_perf_policy_last_render_us(void)
{
    return s_last_render_us;
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
    s_last_render_us = elapsed_us;
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


void ui_perf_policy_record_chunk_draw(uint64_t draw_us, uint32_t pixels)
{
    s_stage_metrics.chunk_draw_total_us += draw_us;
    if (draw_us > s_stage_metrics.chunk_draw_max_us) {
        s_stage_metrics.chunk_draw_max_us = draw_us;
    }
    s_stage_metrics.chunk_pixels_total += pixels;
    if (s_stage_metrics.chunk_samples < UINT32_MAX) {
        ++s_stage_metrics.chunk_samples;
    }
}


void ui_perf_policy_record_frame_chunks(uint32_t chunks)
{
    if (chunks > s_stage_metrics.frame_chunks_max) {
        s_stage_metrics.frame_chunks_max = chunks;
    }
}


ui_perf_profile_t ui_perf_policy_profile(const ui_perf_stage_metrics_t *metrics)
{
    ui_perf_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    if (metrics == NULL) return profile;

    if (metrics->render_samples > 0U) {
        const uint64_t x10 = (uint64_t)metrics->chunk_samples * 10U / metrics->render_samples;
        profile.chunks_per_frame_x10 = x10 > UINT32_MAX ? UINT32_MAX : (uint32_t)x10;
    }
    profile.chunks_per_frame_max = metrics->frame_chunks_max;
    profile.chunk_draw_max_us = metrics->chunk_draw_max_us;
    if (metrics->chunk_samples > 0U) {
        profile.chunk_draw_avg_us = metrics->chunk_draw_total_us / metrics->chunk_samples;
        profile.pixels_per_chunk = (uint32_t)(metrics->chunk_pixels_total / metrics->chunk_samples);
    }
    if (metrics->chunk_pixels_total > 0U) {
        const uint64_t ns = metrics->chunk_draw_total_us * 1000U / metrics->chunk_pixels_total;
        profile.draw_ns_per_pixel = ns > UINT32_MAX ? UINT32_MAX : (uint32_t)ns;
    }
    return profile;
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
