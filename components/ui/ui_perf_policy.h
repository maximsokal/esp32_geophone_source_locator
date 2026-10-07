#pragma once

#include <stdbool.h>
#include <stdint.h>


#define UI_PERF_LOG_HEARTBEAT_MS 60000U


typedef struct
{
    bool has_logged;
    uint32_t last_log_ms;
    uint64_t last_frames_rendered;
    uint64_t last_elapsed_us;
    uint64_t last_max_render_us;
    uint32_t last_target_fps;
} ui_perf_policy_state_t;


bool ui_perf_policy_should_log(
    ui_perf_policy_state_t *state,
    uint32_t now_ms,
    uint64_t frames_rendered,
    uint64_t elapsed_us,
    uint32_t target_fps,
    uint64_t max_render_us
);
