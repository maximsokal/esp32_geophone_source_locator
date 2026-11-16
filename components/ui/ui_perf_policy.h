#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define UI_PERF_LOG_HEARTBEAT_MS 60000U
#define UI_PERF_RENDER_WARNING_US 50000U /* Conservative default pending HIL. */
#define UI_PERF_STARVATION_BUDGET_US 150000U /* Conservative default pending HIL. */
/*
 * The screen is a side show next to the acquisition, the recorder and the SD card, and it shares SPI2 with the card:
 * it never redraws more than UI_PERF_TARGET_FPS times a second, on every screen. While a client talks to the web
 * API the picture on the device matters even less, and it drops to UI_PERF_REMOTE_CLIENT_TARGET_FPS. A key press is
 * answered with a frame at the next 4 FPS tick instead of waiting for the slow one. The LVGL task also runs below
 * the storage, recorder and measurement tasks (app_task_config.h).
 */
#define UI_PERF_TARGET_FPS 4U
#define UI_PERF_REMOTE_CLIENT_TARGET_FPS 1U
/* A client counts as active for this long after its last HTTP request reached the device. */
#define UI_PERF_REMOTE_ACTIVITY_WINDOW_MS 10000U
/*
 * A frame waits for queued SD operations, but never longer than this (two ticks): a recording keeps the card busy for
 * minutes, and a key press must not wait so long that it expires (ui_event_policy.h).
 */
#define UI_PERF_STORAGE_DEFER_MAX_US 500000U
/*
 * LVGL counts time in 10 ms ticks (ui_lvgl.c), so its refresh timer can run up to one tick early against the
 * microsecond clock. A run that much early still counts as due: without it a tick at 249 ms would be skipped and the
 * screen would drop to 2 FPS. The price is a minimum spacing of 240 ms between frames, not 250.
 */
#define UI_PERF_FRAME_JITTER_US 10000U
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
 * (priority 2, below LVGL's 4 at the time) did not run for up to 58 s, every node went stale, workflow_poll missed 65
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
    /* Per flush chunk: the time LVGL needed to draw it (without the transfer), and the pixels it covered. */
    uint64_t chunk_draw_total_us;
    uint64_t chunk_draw_max_us;
    uint64_t chunk_pixels_total;
    uint32_t chunk_samples;
    uint32_t frame_chunks_max;
} ui_perf_stage_metrics_t;


/*
 * What one log window says about where the render time goes. A frame costs (chunks per frame) x (cost of a chunk),
 * and a chunk costs a fixed part (LVGL walks the whole object tree for every chunk) plus a part per pixel (blending,
 * text). Few pixels per chunk with a high draw time points at the fixed part; a high ns per pixel with full-width
 * chunks points at slow code or memory (flash cache misses, PSRAM).
 */
typedef struct
{
    uint32_t chunks_per_frame_x10;
    uint32_t chunks_per_frame_max;
    uint64_t chunk_draw_avg_us;
    uint64_t chunk_draw_max_us;
    uint32_t pixels_per_chunk;
    uint32_t draw_ns_per_pixel;
} ui_perf_profile_t;


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
uint32_t ui_perf_policy_target_fps(bool remote_client);
uint32_t ui_perf_policy_update_interval_ms(bool remote_client);
uint64_t ui_perf_policy_update_interval_us(bool remote_client);
/*
 * The time from one periodic update to the next: the target interval, or on the live-chart screens the rest after a
 * costly frame if that is longer.
 */
uint64_t ui_perf_policy_update_gap_us(bool remote_client, bool load_limited, uint64_t last_render_us);
/* True while the last HTTP request is younger than UI_PERF_REMOTE_ACTIVITY_WINDOW_MS (wrap-safe 32-bit ms clock). */
bool ui_perf_policy_remote_client_active(bool seen, uint32_t last_request_ms, uint32_t now_ms);
uint64_t ui_perf_policy_last_render_us(void);
bool ui_perf_policy_chart_refresh_due(uint64_t elapsed_us, bool load_limited);
size_t ui_perf_policy_preview_frame_limit(size_t requested_frames, bool load_limited);
size_t ui_perf_policy_preview_chunk_limit(size_t requested_frames, bool load_limited);
bool ui_perf_policy_data_prep_budget_exhausted(uint64_t elapsed_us);
bool ui_perf_policy_render_warning(uint64_t max_render_us);
bool ui_perf_policy_starvation_window(uint64_t max_render_us);
/*
 * Paces what the LVGL refresh timer does. Three rules, in this order:
 *  1. nothing runs less than the 4 FPS interval after the previous frame (the ceiling; also bounds how fast a
 *     burst of key presses can redraw);
 *  2. nothing runs while an SD operation is queued or running, for at most UI_PERF_STORAGE_DEFER_MAX_US;
 *  3. a periodic refresh (new data) runs at its own, slower interval; a key press runs at the next tick of rule 1.
 */
typedef struct
{
    uint64_t floor_due_us;
    uint64_t periodic_due_us;
    uint64_t defer_started_us;
    bool deferring;
} ui_perf_pacer_t;

typedef enum
{
    UI_PERF_PACER_WAIT = 0,   /* under the ceiling: do nothing */
    UI_PERF_PACER_DEFER,      /* an SD operation is queued: do nothing, look again at the next tick */
    UI_PERF_PACER_RUN         /* handle input; refresh if input arrived or ui_perf_pacer_periodic_due() */
} ui_perf_pacer_step_t;

void ui_perf_pacer_init(ui_perf_pacer_t *pacer, uint64_t now_us);
ui_perf_pacer_step_t ui_perf_pacer_begin(ui_perf_pacer_t *pacer, uint64_t now_us, bool storage_pending);
bool ui_perf_pacer_periodic_due(const ui_perf_pacer_t *pacer, uint64_t now_us);
/* Call after a run in which a frame was drawn; now_us is the time the run started. */
void ui_perf_pacer_frame_done(
    ui_perf_pacer_t *pacer,
    uint64_t now_us,
    bool periodic_ran,
    bool remote_client,
    bool load_limited,
    uint64_t last_render_us
);
/* The next periodic refresh comes at once (a live-chart screen has just opened); the ceiling still holds. */
void ui_perf_pacer_periodic_now(ui_perf_pacer_t *pacer, uint64_t now_us);

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
void ui_perf_policy_record_chunk_draw(uint64_t draw_us, uint32_t pixels);
void ui_perf_policy_record_frame_chunks(uint32_t chunks);
ui_perf_profile_t ui_perf_policy_profile(const ui_perf_stage_metrics_t *metrics);
ui_perf_stage_metrics_t ui_perf_policy_stage_snapshot(void);
ui_perf_stage_metrics_t ui_perf_policy_stage_take(void);
