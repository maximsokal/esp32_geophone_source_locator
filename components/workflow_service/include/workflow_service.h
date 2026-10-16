#pragma once

#include "esp_err.h"
#include "workflow_core.h"

#define WORKFLOW_SERVICE_POLL_PERIOD_MS 200U

typedef bool (*workflow_service_placement_ready_fn)(void *context);

typedef struct {
    uint32_t peak_occupancy;
    uint32_t overflow_count;
} workflow_service_queue_stats_t;

esp_err_t workflow_service_init(void);
void workflow_service_shutdown(void);
esp_err_t workflow_service_poll(void);
esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot);
esp_err_t workflow_service_try_get_snapshot(workflow_snapshot_t *out);
esp_err_t workflow_service_get_queue_stats(workflow_service_queue_stats_t *stats);
esp_err_t workflow_service_command_async(
    workflow_command_t command,
    bool has_generation,
    uint32_t expected_generation,
    uint32_t *out_seq
);
workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
);
esp_err_t workflow_service_set_placement_ready_predicate(
    workflow_service_placement_ready_fn predicate,
    void *context
);
esp_err_t workflow_service_set_progress(int progress);
esp_err_t workflow_service_notify_calc_settings_saved(const char *object_id);
esp_err_t workflow_service_save_calc_material_async(
    const char *material_id,
    uint32_t custom_v_mps,
    uint32_t custom_sigma_mps
);
esp_err_t workflow_service_save_calc_material_range_async(
    const char *material_id,
    uint32_t custom_v_mps,
    uint32_t custom_sigma_mps,
    bool has_range,
    uint32_t range_min_mps,
    uint32_t range_max_mps
);
esp_err_t workflow_service_capture_height_references(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation
);
