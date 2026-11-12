#include "nrf_emergency_stop_monitor.h"

#include <stdio.h>
#include <string.h>

#include "app_task_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nrf_emergency_stop_policy.h"
#include "nrf_topology_service.h"
#include "runtime_observability.h"
#include "seismic_recorder.h"

#define NRF_EMERGENCY_STOP_MONITOR_ACTIVE_PERIOD_MS 2000U
#define NRF_EMERGENCY_STOP_MONITOR_IDLE_PERIOD_MS   1000U


static const char *TAG = "NRF_ESTOP";


static void capture_channel_samples(
    nrf_emergency_stop_channel_sample_t samples[NRF_TOPOLOGY_REMOTE_COUNT]
)
{
    memset(samples, 0, sizeof(nrf_emergency_stop_channel_sample_t) * NRF_TOPOLOGY_REMOTE_COUNT);

    nrf_topology_snapshot_t topology;
    if (nrf_topology_service_get_snapshot(&topology) != ESP_OK) {
        return;
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        nrf_remote_sensor_cache_t cache;
        if (nrf_topology_service_get_sensor_snapshot(channel, &cache) != ESP_OK) {
            continue;
        }
        if (!cache.response_valid || !cache.topology_current) {
            continue;
        }
        samples[channel].valid = true;
        samples[channel].altitude_mm = cache.sensor.altitude_mm;
        samples[channel].accel_mg[0] = cache.sensor.accel_mg[0];
        samples[channel].accel_mg[1] = cache.sensor.accel_mg[1];
        samples[channel].accel_mg[2] = cache.sensor.accel_mg[2];
    }
}


static void monitor_task(void *argument)
{
    (void)argument;

    bool have_baseline = false;
    char baseline_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE] = "";
    nrf_emergency_stop_channel_sample_t baseline[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(baseline, 0, sizeof(baseline));

    for (;;) {
        seismic_recorder_snapshot_t recorder;
        const bool recording =
            seismic_recorder_get_snapshot(&recorder) == ESP_OK &&
            recorder.running &&
            recorder.state == SEISMIC_RECORDER_RECORDING;

        if (!recording) {
            have_baseline = false;
            vTaskDelay(pdMS_TO_TICKS(NRF_EMERGENCY_STOP_MONITOR_IDLE_PERIOD_MS));
            continue;
        }

        /* Ask for a fresh read, then give nrf_topology_service's own task a
         * cycle to actually perform it before trusting the cache. */
        (void)nrf_topology_service_request_all_sensor_refresh();
        vTaskDelay(pdMS_TO_TICKS(NRF_EMERGENCY_STOP_MONITOR_ACTIVE_PERIOD_MS));

        if (!have_baseline || strcmp(baseline_session_id, recorder.session_id) != 0) {
            capture_channel_samples(baseline);
            snprintf(baseline_session_id, sizeof(baseline_session_id), "%s", recorder.session_id);
            have_baseline = true;
            continue;
        }

        nrf_emergency_stop_channel_sample_t current[NRF_TOPOLOGY_REMOTE_COUNT];
        capture_channel_samples(current);

        size_t triggered_channel = 0U;
        if (
            nrf_emergency_stop_any_channel_triggered(
                baseline, current, NRF_TOPOLOGY_REMOTE_COUNT, &triggered_channel)
        ) {
            ESP_LOGE(
                TAG,
                "EMERGENCY STOP: pod channel=%u moved (altitude jump or multi-axis shift) -- "
                "stopping whole recording object=%s session=%s",
                (unsigned)triggered_channel,
                recorder.object_id,
                recorder.session_id
            );
            (void)seismic_recorder_stop();
            have_baseline = false;
        }
    }
}


esp_err_t nrf_emergency_stop_monitor_start(void)
{
    TaskHandle_t task = NULL;
    runtime_observability_log_memory("estop_before_task_create");
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        monitor_task,
        "nrf_estop_monitor",
        APP_TASK_STACK_NRF_ESTOP_MONITOR,
        NULL,
        APP_TASK_PRIORITY_NRF_ESTOP_MONITOR,
        &task,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created != pdPASS) {
        runtime_observability_log_memory("estop_task_create_failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Emergency stop monitor started on core=%d stack_bytes=%u stack_caps=spiram|8bit",
             APP_TASK_CORE_SERVICES, (unsigned)APP_TASK_STACK_NRF_ESTOP_MONITOR);
    return ESP_OK;
}
