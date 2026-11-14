#include "app_test_recording.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_service.h"
#include "runtime_observability.h"

#define TEST_RECORDING_SETTLE_MS 12000U
#define TEST_RECORDING_RECORD_MS 10000U
#define TEST_RECORDING_POLL_MS 200U

static const char *TAG = "TEST_RECORDING";

static void fail(const char *stage, esp_err_t err)
{
    ESP_LOGE(TAG, "FAIL stage=%s err=%s", stage, esp_err_to_name(err));
}

static bool wait_state(measurement_service_state_t wanted, uint32_t timeout_ms, measurement_service_snapshot_t *out)
{
    for (uint32_t waited = 0U; waited <= timeout_ms; waited += TEST_RECORDING_POLL_MS) {
        if (measurement_service_get_snapshot(out) == ESP_OK) {
            if (out->state == wanted) return true;
            if (out->state == MEASUREMENT_SERVICE_ERROR) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_POLL_MS));
    }
    return false;
}

static void scenario_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_SETTLE_MS));
    ESP_LOGI(TAG, "begin");

    esp_err_t err = object_service_create_async();
    if (err != ESP_OK) { fail("create_object", err); goto done; }
    object_service_summary_t object = {0};
    bool have_object = false;
    for (uint32_t waited = 0U; waited < 20000U; waited += TEST_RECORDING_POLL_MS) {
        if (object_service_get_active(&object) == ESP_OK && object.active && object.id[0] != '\0') {
            have_object = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_POLL_MS));
    }
    if (!have_object) { fail("active_object", ESP_ERR_TIMEOUT); goto done; }
    ESP_LOGI(TAG, "object=%s", object.id);

    err = nrf_topology_service_test_seed_confirmed_setup(object.id);
    if (err != ESP_OK) { fail("seed_setup", err); goto done; }
    ESP_LOGI(TAG, "setup seeded");

    err = measurement_service_start_async();
    if (err != ESP_OK) { fail("start_async", err); goto done; }
    measurement_service_snapshot_t snapshot = {0};
    if (!wait_state(MEASUREMENT_SERVICE_RECORDING, 30000U, &snapshot)) {
        ESP_LOGE(TAG, "FAIL stage=recording state=%d last_error=%s", (int)snapshot.state,
                 esp_err_to_name(snapshot.last_error));
        goto done;
    }
    ESP_LOGI(TAG, "recording started session=%s", snapshot.session_id);

    vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_RECORD_MS));
    runtime_observability_request_snapshot("recording-scenario-peak");
    (void)measurement_service_get_snapshot(&snapshot);
    ESP_LOGI(TAG, "recording samples=%llu dropped=%llu ms=%llu", (unsigned long long)snapshot.samples_written,
             (unsigned long long)snapshot.dropped_frames, (unsigned long long)snapshot.recording_ms);

    err = measurement_service_stop_async();
    if (err != ESP_OK) { fail("stop_async", err); goto done; }
    if (!wait_state(MEASUREMENT_SERVICE_REVIEW, 60000U, &snapshot)) {
        ESP_LOGE(TAG, "FAIL stage=stop state=%d last_error=%s stop_error=%s", (int)snapshot.state,
                 esp_err_to_name(snapshot.last_error), esp_err_to_name(snapshot.stop_stage_error));
        goto done;
    }
    ESP_LOGI(TAG, "stopped state=REVIEW samples=%llu dropped=%llu", (unsigned long long)snapshot.samples_written,
             (unsigned long long)snapshot.dropped_frames);
    runtime_observability_request_snapshot("recording-scenario-end");
    vTaskDelay(pdMS_TO_TICKS(1500U));
    ESP_LOGI(TAG, "DONE");

done:
    vTaskDeleteWithCaps(NULL);
}

void app_test_recording_scenario_start(void)
{
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        scenario_task, "test_recording", 6144U, NULL, tskIDLE_PRIORITY + 2U, NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) ESP_LOGE(TAG, "FAIL stage=task_create err=ESP_ERR_NO_MEM");
}
