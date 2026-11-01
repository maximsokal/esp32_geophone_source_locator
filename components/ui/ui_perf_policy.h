#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define UI_PERF_LOG_HEARTBEAT_MS 60000U
#define UI_PERF_RENDER_WARNING_US 50000U /* Conservative default pending HIL. */
#define UI_PERF_STARVATION_BUDGET_US 150000U /* Conservative default pending HIL. */
#define UI_PERF_DEFAULT_TARGET_FPS 24U /* Conservative default pending HIL. */
#define UI_PERF_MEASUREMENTS_TARGET_FPS 4U /* Conservative default pending HIL. */
#define UI_PERF_CHART_REFRESH_INTERVAL_MS 125U /* Conservative default pending HIL. */
#define UI_PERF_PREVIEW_MAX_FRAMES 4096U /* Conservative default pending HIL. */
#define UI_PERF_PREVIEW_CHUNK_FRAMES 1024U /* Conservative default pending HIL. */
#define UI_PERF_LIMITED_PREVIEW_CHUNK_FRAMES 128U /* Conservative default pending HIL. */
#define UI_PERF_FLUSH_CHUNK_ROWS 8U /* Conservative default pending HIL. */
#define UI_PERF_DATA_PREP_BUDGET_US 50000U /* Conservative default pending HIL. */
#define UI_PERF_FLUSH_CHUNK_BUDGET_US 5000U /* Conservative default pending HIL. */
#define UI_PERF_POLICY_HAS_MEASUREMENT_LOAD_GUARDS 1
/*
 * On the live charts the LVGL task rested only for the 250 ms of the 4 FPS target. A frame there costs ~340 ms of
 * core 0 (bench log 2026-11-01 21:46: taskLVGL 66 % of core 0, IDLE0 3 %, the task watchdog fired, nrf_topology
 * (priority 2, below LVGL's 4) did not run for up to 58 s, every node went stale, workflow_poll missed 65
 * deadlines). The task now rests this many times the cost of its last frame, so LVGL holds about a third of the
 * core at most and the sensor bus services get the rest; the picture updates less often instead.
 */
#define UI_PERF_LOAD_REST_FACTOR 2U
#define UI_PERF_LOAD_REST_MAX_US 800000U


typedef struct
{
    bool has_logged;
    uint32_t last_log_ms;
    uint64_t last_frames_rendered;
    uint64_t last_elapsed_us;
    uint64_t last_max_render_us;
    uint32_t last_target_fps;
    uint32_t starvation_windows;
} ui_perf_policy_state_t;


typedef struct
{
    uint64_t update_total_us;
    uint64_t update_max_us;
    uint64_t update_samples;
    uint64_t render_total_us;
    uint64_t render_max_us;
    uint64_t render_samples;
    uint64_t flush_submit_total_us;
    uint64_t flush_submit_max_us;
    uint32_t flush_chunks;
    uint64_t flush_wait_total_us;
    uint64_t flush_wait_max_us;
    uint32_t flush_waits;
    uint32_t flush_yields;
    uint32_t flush_submit_over_budget_chunks;
    uint64_t data_prep_total_us;
    uint64_t data_prep_max_us;
    uint64_t data_prep_samples;
} ui_perf_stage_metrics_t;


bool ui_perf_policy_should_log(
    ui_perf_policy_state_t *state,
    uint32_t now_ms,
    uint64_t frames_rendered,
    uint64_t elapsed_us,
    uint32_t target_fps,
    uint64_t max_render_us
);

bool ui_perf_policy_measurements_load_active(
    bool measurements_screen_active,
    bool acquisition_running
);

/* Screens that draw the live geophone charts: MEASUREMENTS and the workflow while it is RECORDING. */
bool ui_perf_policy_live_charts_screen(
    bool measurements_screen,
    bool workflow_screen,
    bool workflow_recording
);
uint32_t ui_perf_policy_target_fps(bool load_limited);
uint32_t ui_perf_policy_update_interval_ms(bool load_limited);
uint64_t ui_perf_policy_update_interval_us(bool load_limited);
/* The time from one chart update to the next: the target interval, or the rest after a costly frame if longer. */
uint64_t ui_perf_policy_update_gap_us(bool load_limited, uint64_t last_render_us);
uint64_t ui_perf_policy_last_render_us(void);
bool ui_perf_policy_chart_refresh_due(uint64_t elapsed_us, bool load_limited);
size_t ui_perf_policy_preview_frame_limit(size_t requested_frames, bool load_limited);
size_t ui_perf_policy_preview_chunk_limit(size_t requested_frames, bool load_limited);
bool ui_perf_policy_data_prep_budget_exhausted(uint64_t elapsed_us);
bool ui_perf_policy_render_warning(uint64_t max_render_us);
bool ui_perf_policy_starvation_window(uint64_t max_render_us);
uint64_t ui_perf_policy_flush_wire_time_us(
    uint32_t width,
    uint32_t rows,
    uint32_t bytes_per_pixel,
    uint32_t spi_clock_hz
);
uint32_t ui_perf_policy_flush_chunk_count(uint32_t total_rows, uint32_t rows_per_chunk);
uint32_t ui_perf_policy_flush_yield_count(uint32_t flush_chunks);

void ui_perf_policy_record_update(uint64_t elapsed_us);
void ui_perf_policy_record_render(uint64_t elapsed_us);
void ui_perf_policy_record_flush_submit(uint64_t elapsed_us);
void ui_perf_policy_record_flush_wait(uint64_t elapsed_us, bool yielded);
void ui_perf_policy_record_data_prep(uint64_t elapsed_us);
ui_perf_stage_metrics_t ui_perf_policy_stage_snapshot(void);
ui_perf_stage_metrics_t ui_perf_policy_stage_take(void);
