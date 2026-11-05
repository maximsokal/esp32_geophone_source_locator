#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Keep the existing recorder fixture's scheduler separate from the AD7606
 * runtime fixture, whose scheduler runs the acquisition worker inline. */
#define main(...) max435_existing_recorder_integration_main(__VA_ARGS__)
#define xSemaphoreCreateMutex max435_recorder_xSemaphoreCreateMutex
#define xSemaphoreTake max435_recorder_xSemaphoreTake
#define xSemaphoreGive max435_recorder_xSemaphoreGive
#define vSemaphoreDelete max435_recorder_vSemaphoreDelete
#define xEventGroupCreate max435_recorder_xEventGroupCreate
#define xEventGroupSetBits max435_recorder_xEventGroupSetBits
#define xEventGroupClearBits max435_recorder_xEventGroupClearBits
#define xEventGroupWaitBits max435_recorder_xEventGroupWaitBits
#define vEventGroupDelete max435_recorder_vEventGroupDelete
#define xTaskCreatePinnedToCore max435_recorder_xTaskCreatePinnedToCore
#define vTaskDelay max435_recorder_vTaskDelay
#define runtime_observability_beat max435_recorder_runtime_observability_beat
#define esp_timer_get_time max435_recorder_esp_timer_get_time
#define SEISMIC_RECORDER_HEALTH_PHASE_STUB_EXTERNAL 1
#include "seismic_recorder_start_integration_test.c"
#undef SEISMIC_RECORDER_HEALTH_PHASE_STUB_EXTERNAL
#undef main

#define xSemaphoreCreateMutex max435_recorder_xSemaphoreCreateMutex
#define xSemaphoreTake max435_recorder_xSemaphoreTake
#define xSemaphoreGive max435_recorder_xSemaphoreGive
#define vSemaphoreDelete max435_recorder_vSemaphoreDelete
#define xEventGroupCreate max435_recorder_xEventGroupCreate
#define xEventGroupSetBits max435_recorder_xEventGroupSetBits
#define xEventGroupClearBits max435_recorder_xEventGroupClearBits
#define xEventGroupWaitBits max435_recorder_xEventGroupWaitBits
#define vEventGroupDelete max435_recorder_vEventGroupDelete
#define xTaskCreatePinnedToCore max435_recorder_xTaskCreatePinnedToCore
#define vTaskDelay max435_recorder_vTaskDelay
#define runtime_observability_beat max435_recorder_runtime_observability_beat
#define esp_timer_get_time max435_recorder_esp_timer_get_time
#include "../../components/seismic_recorder/seismic_recorder.c"
#undef xSemaphoreCreateMutex
#undef xSemaphoreTake
#undef xSemaphoreGive
#undef vSemaphoreDelete
#undef xEventGroupCreate
#undef xEventGroupSetBits
#undef xEventGroupClearBits
#undef xEventGroupWaitBits
#undef vEventGroupDelete
#undef xTaskCreatePinnedToCore
#undef vTaskDelay
#undef runtime_observability_beat
#undef esp_timer_get_time

#define esp_timer_get_time max435_recorder_esp_timer_get_time
#include "../../components/seismic_recorder/seismic_recorder_start_policy.c"
#undef esp_timer_get_time

bool ad7606_acquisition_test_start_with_tconv(uint32_t tconv_ns);

int main(void)
{
    if (max435_existing_recorder_integration_main() != 0) {
        puts("MAX435 T8 FAIL existing recorder fixture");
        return 1;
    }

    /* The shared fixture ends with a deliberately rejected 6 m node on a 5 m cable. */
    fake_profile_cable_length_m = 6.0f;
    memset(&fake_acquisition, 0, sizeof(fake_acquisition));
    fake_acquisition.source_name = "AD7606";
    fake_acquisition.sample_rate_hz = 500U;
    fake_acquisition.latest_valid = true;
    fake_acquisition.samples = 1U;
    fake_acquisition.last_sample_timestamp_us = 999000;
    fake_acquisition.cadence_available = true;
    const bool acquisition_started =
        ad7606_acquisition_test_start_with_tconv(5644U);
    assert(acquisition_started);
    fake_acquisition.running = acquisition_started;

    const esp_err_t recorder_result = seismic_recorder_start("OBJ_0007");
    seismic_recorder_snapshot_t recorder_status = {0};
    const esp_err_t snapshot_result = seismic_recorder_get_snapshot(&recorder_status);
    const bool passed = acquisition_started && recorder_result == ESP_OK &&
        snapshot_result == ESP_OK && recorder_status.running &&
        recorder_status.state == SEISMIC_RECORDER_RECORDING;
    if (passed) {
        puts("MAX435 T8 PASS");
        return 0;
    }

    printf("MAX435 T8 FAIL acquisition_started=%d recorder_result=%d state=%d running=%d\n",
        acquisition_started ? 1 : 0,
        (int)recorder_result,
        (int)recorder_status.state,
        recorder_status.running ? 1 : 0);
    return 1;
}
