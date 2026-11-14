#include "seismic_test_generator.h"

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>

#include "app_task_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
/* 500 Hz on a 10 ms tick: five frames per wake-up, stamped 2 ms apart, like the ADC delivers them. */
#define TEST_PERIOD_MS       10
#define TEST_FRAMES_PER_TICK (SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ * TEST_PERIOD_MS / 1000)
#define TEST_PHYSICAL_EVENT  1
#else
#define TEST_PERIOD_MS       (1000 / SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ)
#define TEST_FRAMES_PER_TICK 1
#define TEST_PHYSICAL_EVENT  0
#define TEST_EVENT_PERIOD    200
#define TEST_EVENT_START     40
#endif


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


#if TEST_PHYSICAL_EVENT
/*
 * One impact at a known place: each sensor sees the pulse after distance / speed, weaker the farther it is. The
 * arrival is fractional in frames, so the delays between channels are exact rather than rounded to a sample.
 */
static float s_arrival_frame[SEISMIC_CHANNEL_COUNT];
static float s_amplitude_uv[SEISMIC_CHANNEL_COUNT];

static void test_physical_event_prepare(void)
{
    static const int node_x_mm[] = TEST_REC_NODE_X_MM;
    static const int node_y_mm[] = TEST_REC_NODE_Y_MM;
    static const int channel_node[SEISMIC_CHANNEL_COUNT] = TEST_REC_CHANNEL_NODE;
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        s_arrival_frame[channel] = -1.0f;
        s_amplitude_uv[channel] = 0.0f;
        if (channel_node[channel] < 0) continue;
        const double dx = (double)(node_x_mm[channel_node[channel]] - TEST_REC_SOURCE_X_MM);
        const double dy = (double)(node_y_mm[channel_node[channel]] - TEST_REC_SOURCE_Y_MM);
        const double dz = (double)TEST_REC_SOURCE_Z_MM;
        const double distance_mm = sqrt(dx * dx + dy * dy + dz * dz);
        const double delay_s = distance_mm / 1000.0 / TEST_REC_SPEED_MPS;
        s_arrival_frame[channel] = (float)(TEST_REC_EVENT_FRAME + delay_s * SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ);
        s_amplitude_uv[channel] = (float)(300.0 * 1000.0 / distance_mm);
    }
}

static float test_event_uv(
    uint64_t frame_number,
    size_t channel
)
{
    if (s_arrival_frame[channel] < 0.0f) return 0.0f;
    const size_t pulse_count = sizeof(s_pulse) / sizeof(s_pulse[0]);
    const float x = (float)frame_number - s_arrival_frame[channel];
    if (x < 0.0f || x >= (float)(pulse_count - 1U)) return 0.0f;
    const size_t index = (size_t)x;
    const float fraction = x - (float)index;
    return (s_pulse[index] * (1.0f - fraction) + s_pulse[index + 1U] * fraction) * s_amplitude_uv[channel];
}
#else
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
#endif


static void seismic_test_task(
    void *argument
)
{
    (void)argument;

    TickType_t last_wake =
        xTaskGetTickCount();

    uint64_t frame_number =
        0;
    int64_t last_stamp_us = 0;

    while (s_running) {
        const int64_t tick_us = esp_timer_get_time();
        for (int k = 0; k < TEST_FRAMES_PER_TICK; ++k) {
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

            /* Follow the wall clock, but never step back or stand still: a wake-up that comes late must not make
             * the next frames older than the last one (the calculation rejects a non-monotonic trace). */
            const int64_t frame_period_us = 1000000 / SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ;
            int64_t stamp_us = tick_us - (int64_t)(TEST_FRAMES_PER_TICK - 1 - k) * frame_period_us;
            if (last_stamp_us != 0 && stamp_us <= last_stamp_us) stamp_us = last_stamp_us + frame_period_us;
            last_stamp_us = stamp_us;
            seismic_buffer_push(
                stamp_us,
                channels
            );

            frame_number++;
        }

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
            SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ
        )
    );
#if TEST_PHYSICAL_EVENT
    test_physical_event_prepare();
#endif

    s_running =
        true;

    BaseType_t result =
        xTaskCreatePinnedToCore(
            seismic_test_task,
            "seismic_test",
            APP_TASK_STACK_SEISMIC_TEST,
            NULL,
            APP_TASK_PRIORITY_SEISMIC_TEST,
            &s_task,
            APP_TASK_CORE_REALTIME
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
        "TEST waveform generator enabled at %d Hz core=%d",
        SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ,
        APP_TASK_CORE_REALTIME
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
