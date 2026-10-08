#include "ui_perf_policy.h"


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
    heartbeat_due = state->has_logged &&
        (uint32_t)(now_ms - state->last_log_ms) >=
            UI_PERF_LOG_HEARTBEAT_MS;
    should_log =
        !state->has_logged || low_fps || over_frame_budget || heartbeat_due;
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
