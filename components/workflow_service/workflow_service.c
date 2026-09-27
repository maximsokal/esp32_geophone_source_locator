#include "workflow_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_service.h"

static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static bool s_recorder_reconciliation_armed;
static uint8_t s_assigned_remotes;
static char s_active_object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
static workflow_core_t s_core;
static workflow_service_placement_ready_fn s_placement_ready_predicate;
static void *s_placement_ready_context;

static bool workflow_service_lock(void)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void workflow_service_unlock(void)
{
    (void)xSemaphoreGive(s_mutex);
}

static bool workflow_service_placement_ready(void)
{
    return s_placement_ready_predicate != NULL &&
        s_placement_ready_predicate(s_placement_ready_context);
}

static uint8_t workflow_service_assigned_remote_count(void)
{
    nrf_topology_snapshot_t topology;
    if (nrf_topology_service_get_snapshot(&topology) != ESP_OK) return 0U;

    size_t count = 0U;
    for (size_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        if (topology.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED) ++count;
    }
    return count > UINT8_MAX ? UINT8_MAX : (uint8_t)count;
}

static esp_err_t workflow_service_active_object(
    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE],
    bool *active_out
)
{
    if (object_id == NULL || active_out == NULL) return ESP_ERR_INVALID_ARG;
    object_id[0] = '\0';
    *active_out = false;
    object_service_summary_t object;
    const esp_err_t err = object_service_get_active(&object);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (!object.active || object.id[0] == '\0') return ESP_OK;
    (void)snprintf(object_id, OBJECT_SERVICE_OBJECT_ID_SIZE, "%s", object.id);
    *active_out = true;
    return ESP_OK;
}

static void workflow_service_reconcile_locked(void)
{
    measurement_service_snapshot_t measurement;
    const esp_err_t measurement_err = measurement_service_get_snapshot(&measurement);

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = {0};
    bool active_object_present = false;
    if (workflow_service_active_object(object_id, &active_object_present) == ESP_OK &&
        strcmp(object_id, s_active_object_id) != 0) {
        (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
        workflow_core_reset_object(&s_core, active_object_present, true);
    }

    s_assigned_remotes = workflow_service_assigned_remote_count();
    if (measurement_err != ESP_OK) return;

    if (measurement.state == MEASUREMENT_SERVICE_LIVE) {
        s_recorder_reconciliation_armed = true;
        return;
    }
    if (!s_recorder_reconciliation_armed) return;

    if (measurement.state == MEASUREMENT_SERVICE_STARTING ||
        measurement.state == MEASUREMENT_SERVICE_RECORDING) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_RECORDING, NULL);
    }
    else if (measurement.state == MEASUREMENT_SERVICE_REVIEW &&
        s_core.step == WORKFLOW_STEP_RECORDING) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_STOPPED, NULL);
    }
    else if (measurement.state == MEASUREMENT_SERVICE_ERROR &&
        (s_core.step == WORKFLOW_STEP_RECORDING || s_core.step == WORKFLOW_STEP_STOPPED)) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_STOPPED, "recording_error");
    }
}

esp_err_t workflow_service_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = {0};
    bool active_object_present = false;
    (void)workflow_service_active_object(object_id, &active_object_present);
    (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
    workflow_core_init(&s_core, active_object_present);
    s_assigned_remotes = workflow_service_assigned_remote_count();
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;

    measurement_service_snapshot_t measurement;
    s_recorder_reconciliation_armed =
        measurement_service_get_snapshot(&measurement) == ESP_OK &&
        measurement.state == MEASUREMENT_SERVICE_LIVE;
    s_initialized = true;
    return ESP_OK;
}

void workflow_service_shutdown(void)
{
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_initialized = false;
    s_recorder_reconciliation_armed = false;
    s_assigned_remotes = 0U;
    s_active_object_id[0] = '\0';
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;
    memset(&s_core, 0, sizeof(s_core));
}

esp_err_t workflow_service_poll(void)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    workflow_service_reconcile_locked();
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    const esp_err_t poll_err = workflow_service_poll();
    if (poll_err != ESP_OK) return poll_err;
    if (!workflow_service_lock()) return ESP_ERR_INVALID_STATE;

    workflow_core_snapshot(
        &s_core,
        s_assigned_remotes,
        workflow_service_placement_ready(),
        snapshot
    );
    workflow_service_unlock();
    return ESP_OK;
}

workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
)
{
    if (params == NULL) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    if (!s_initialized || !workflow_service_lock()) return WORKFLOW_COMMAND_RESULT_UNAVAILABLE;
    workflow_service_reconcile_locked();

    const workflow_core_t previous = s_core;
    const workflow_command_result_t result = workflow_core_apply(
        &s_core,
        command,
        params,
        s_assigned_remotes,
        workflow_service_placement_ready()
    );
    if (result != WORKFLOW_COMMAND_RESULT_OK) {
        workflow_service_unlock();
        return result;
    }

    esp_err_t measurement_err = ESP_OK;
    if (command == WORKFLOW_COMMAND_START_RECORDING) {
        measurement_err = measurement_service_start_async();
        if (measurement_err == ESP_OK) s_recorder_reconciliation_armed = true;
    }
    else if (command == WORKFLOW_COMMAND_STOP_RECORDING) {
        measurement_err = measurement_service_stop_async();
    }
    else if (command == WORKFLOW_COMMAND_DELETE_RECORDING) {
        measurement_err = measurement_service_delete_async();
    }
    if (measurement_err != ESP_OK) {
        s_core = previous;
        workflow_service_unlock();
        return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    }

    workflow_service_unlock();
    return WORKFLOW_COMMAND_RESULT_OK;
}

esp_err_t workflow_service_set_placement_ready_predicate(
    workflow_service_placement_ready_fn predicate,
    void *context
)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    s_placement_ready_predicate = predicate;
    s_placement_ready_context = context;
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_set_progress(int progress)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    const bool updated = workflow_core_set_progress(&s_core, progress);
    workflow_service_unlock();
    return updated ? ESP_OK : ESP_ERR_INVALID_ARG;
}
