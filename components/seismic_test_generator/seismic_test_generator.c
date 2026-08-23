#include "seismic_test_generator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define TEST_SAMPLE_RATE_HZ  50
#define TEST_PERIOD_MS       20
#define TEST_EVENT_PERIOD    200
#define TEST_EVENT_START     40


static const char *TAG =
    "SEISMIC_TEST";


static TaskHandle_t s_task =
    NULL;


static volatile bool s_running =
    false;


static uint32_t s_random_state =
    0x7A31D5C9;


static const float s_pulse[] = {
     0.00f,
     0.10f,
     0.24f,
     0.48f,
     0.78f,
     1.00f,
     0.72f,
     0.18f,
    -0.52f,
    -1.00f,
    -0.82f,
    -0.28f,
     0.42f,
     0.78f,
     0.54f,
     0.08f,
    -0.38f,
    -0.62f,
    -0.40f,
    -0.06f,
     0.24f,
     0.34f,
     0.18f,
     0.02f
};


static uint32_t test_random(void)
{
    uint32_t value =
        s_random_state;


    value ^=
        value << 13;

    value ^=
        value >> 17;

    value ^=
        value << 5;


    s_random_state =
        value;


    return value;
}


static float test_noise_uv(void)
{
    uint32_t value =
        test_random() & 0xFFFF;


    float normalized =
        (float)value /
        65535.0f;


    return
        (normalized * 3.0f) -
        1.5f;
}


static float test_event_uv(
    uint64_t frame_number,
    size_t channel
)
{
    const size_t pulse_count =
        sizeof(s_pulse) /
        sizeof(s_pulse[0]);


    uint64_t phase =
        frame_number %
        TEST_EVENT_PERIOD;


    uint64_t channel_start =
        TEST_EVENT_START +
        (channel * 3);


    if (
        phase < channel_start ||
        phase >=
            channel_start +
            pulse_count
    ) {
        return 0.0f;
    }


    size_t pulse_index =
        (size_t)(
            phase -
            channel_start
        );


    static const float amplitude_uv[
        SEISMIC_CHANNEL_COUNT
    ] = {
        120.0f,
        100.0f,
        82.0f,
        68.0f
    };


    return
        s_pulse[pulse_index] *
        amplitude_uv[channel];
}


static void seismic_test_task(
    void *argument
)
{
    (void)argument;


    TickType_t last_wake =
        xTaskGetTickCount();


    uint64_t frame_number =
        0;


    while (s_running) {

        float channels[
            SEISMIC_CHANNEL_COUNT
        ];


        for (
            size_t channel = 0;
            channel < SEISMIC_CHANNEL_COUNT;
            ++channel
        ) {
            channels[channel] =
                test_noise_uv() +
                test_event_uv(
                    frame_number,
                    channel
                );
        }


        seismic_buffer_push(
            esp_timer_get_time(),
            channels
        );


        frame_number++;


        vTaskDelayUntil(
            &last_wake,
            pdMS_TO_TICKS(
                TEST_PERIOD_MS
            )
        );
    }


    s_task =
        NULL;


    vTaskDelete(
        NULL
    );
}


esp_err_t seismic_test_generator_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }


    ESP_ERROR_CHECK(
        seismic_buffer_configure(
            SEISMIC_SOURCE_TEST,
            TEST_SAMPLE_RATE_HZ
        )
    );


    s_running =
        true;


    BaseType_t result =
        xTaskCreate(
            seismic_test_task,
            "seismic_test",
            3072,
            NULL,
            4,
            &s_task
        );


    if (result != pdPASS) {

        s_running =
            false;

        s_task =
            NULL;

        return ESP_ERR_NO_MEM;
    }


    ESP_LOGW(
        TAG,
        "TEST waveform generator enabled at %d Hz",
        TEST_SAMPLE_RATE_HZ
    );


    return ESP_OK;
}


void seismic_test_generator_stop(void)
{
    if (s_task == NULL) {
        return;
    }


    s_running =
        false;


    while (s_task != NULL) {

        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }


    ESP_LOGI(
        TAG,
        "TEST waveform generator stopped"
    );
}