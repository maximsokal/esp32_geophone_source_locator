#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "nrf_emergency_stop_monitor.h"
#include "nrf_topology_service.h"
#include "seismic_recorder.h"

static bool s_fail_caps;
static unsigned s_caps_attempts;
static TaskFunction_t s_captured_task;
static unsigned s_before_task_stages, s_task_failed_stages;

void runtime_observability_log_memory(const char *stage)
{
    if (strcmp(stage, "estop_before_task_create") == 0) ++s_before_task_stages;
    if (strcmp(stage, "estop_task_create_failed") == 0) ++s_task_failed_stages;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, unsigned stack_size,
                                  void *argument, unsigned priority, TaskHandle_t *task, int core)
{
    (void)entry; (void)name; (void)argument; (void)priority; (void)task; (void)core;
    return stack_size <= 3584U ? pdPASS : 0;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry, const char *name, unsigned stack_size,
                                          void *argument, unsigned priority, TaskHandle_t *task,
                                          int core, UBaseType_t caps)
{
    (void)name;
    assert(entry != NULL && argument == NULL && task != NULL);
    assert(stack_size == 4096U && priority == APP_TASK_PRIORITY_NRF_ESTOP_MONITOR &&
           core == APP_TASK_CORE_SERVICES && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_caps_attempts;
    if (s_fail_caps) return 0;
    s_captured_task = entry;
    *task = (TaskHandle_t)&s_captured_task;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{ (void)snapshot; return ESP_ERR_INVALID_STATE; }
esp_err_t seismic_recorder_stop(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{ (void)snapshot; return ESP_ERR_INVALID_STATE; }
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot)
{ (void)channel; (void)snapshot; return ESP_ERR_INVALID_STATE; }
esp_err_t nrf_topology_service_request_all_sensor_refresh(void) { return ESP_ERR_INVALID_STATE; }

int main(void)
{
    assert(nrf_emergency_stop_monitor_start() == ESP_OK);
    assert(s_caps_attempts == 1U && s_captured_task != NULL);
    assert(s_before_task_stages == 1U && s_task_failed_stages == 0U);
    s_fail_caps = true;
    assert(nrf_emergency_stop_monitor_start() == ESP_ERR_NO_MEM);
    assert(s_caps_attempts == 2U);
    assert(s_before_task_stages == 2U && s_task_failed_stages == 1U);
    s_fail_caps = false;
    assert(nrf_emergency_stop_monitor_start() == ESP_OK);
    assert(s_caps_attempts == 3U && s_captured_task != NULL);
    assert(s_before_task_stages == 3U && s_task_failed_stages == 1U);
    return 0;
}
