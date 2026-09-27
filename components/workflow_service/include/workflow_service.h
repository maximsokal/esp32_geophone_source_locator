#pragma once

#include "esp_err.h"
#include "workflow_core.h"

typedef bool (*workflow_service_placement_ready_fn)(void *context);

esp_err_t workflow_service_init(void);
void workflow_service_shutdown(void);
esp_err_t workflow_service_poll(void);
esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot);
workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
);
esp_err_t workflow_service_set_placement_ready_predicate(
    workflow_service_placement_ready_fn predicate,
    void *context
);
esp_err_t workflow_service_set_progress(int progress);
