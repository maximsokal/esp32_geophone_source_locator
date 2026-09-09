#include "ina3221_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "device_status.h"
#include "ina3221_driver.h"
#include "ina3221_policy.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define INA3221_MONITOR_PERIOD_MS       1000u
#define INA3221_MONITOR_LOG_INTERVAL    5u
#define INA3221_RETRY_LOG_INTERVAL      30u


static const char *TAG =
    "INA3221";


static TaskHandle_t s_task =
    NULL;

static volatile bool s_running =
    false;


static device_power_source_t ina3221_monitor_device_source(
    ina3221_power_state_t state
)
{
    switch (state) {

        case INA3221_POWER_STATE_BATTERY:
            return DEVICE_POWER_SOURCE_BATTERY;

        case INA3221_POWER_STATE_USB:
        case INA3221_POWER_STATE_USB_CHARGING:
            return DEVICE_POWER_SOURCE_USB;

        case INA3221_POWER_STATE_UNKNOWN:
        default:
            return DEVICE_POWER_SOURCE_UNKNOWN;
    }
}


static void ina3221_monitor_publish_invalid(void)
{
    (void)device_status_set_power(
        0.0f,
        0.0f,
        false
    );

    for (
        size_t i = 0;
        i < INA3221_POLICY_CHANNEL_COUNT;
        ++i
    ) {
        (void)device_status_set_power_channel(
            i,
            0.0f,
            0.0f,
            false
        );
    }

    (void)device_status_set_power_battery(
        0.0f,
        0.0f,
        0.0f,
        false,
        false,
        false,
        false,
        DEVICE_POWER_SOURCE_UNKNOWN
    );
}


static void ina3221_monitor_publish(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    const ina3221_power_decision_t *decision
)
{
    for (
        size_t i = 0;
        i < INA3221_POLICY_CHANNEL_COUNT;
        ++i
    ) {
        (void)device_status_set_power_channel(
            i,
            channels[i].bus_voltage_v,
            channels[i].current_a,
            channels[i].valid
        );
    }

    /*
     * The primary power values consumed by existing UI/status code are the
     * actual ESP32 5 V rail at physical INA3221 CH2 / IN- (after the R100
     * shunt). The status array index is the physical channel minus one.
     */
    (void)device_status_set_power(
        channels[1].bus_voltage_v,
        channels[1].current_a,
        channels[1].valid
    );

    (void)device_status_set_power_battery(
        decision->battery_voltage_v,
        decision->battery_current_a,
        decision->battery_percent_estimate,
        decision->battery_present,
        decision->battery_percent_valid,
        decision->usb_present,
        decision->charging,
        ina3221_monitor_device_source(
            decision->state
        )
    );
}


static void ina3221_monitor_log_sample(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    const ina3221_power_decision_t *decision
)
{
    if (decision->battery_present) {
        ESP_LOGI(
            TAG,
            "source=%s INA_CH1_BATTERY=%.3fV %+.3fA %.0f%% usb=%s charging=%s INA_CH2_ESP32_5V=%.3fV %+.3fA INA_CH3_AD7606_5V=%.3fV %+.3fA",
            ina3221_policy_state_name(
                decision->state
            ),
            decision->battery_voltage_v,
            decision->battery_current_a,
            decision->battery_percent_estimate,
            decision->usb_present ? "yes" : "no",
            decision->charging ? "yes" : "no",
            channels[1].bus_voltage_v,
            channels[1].current_a,
            channels[2].bus_voltage_v,
            channels[2].current_a
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "source=%s INA_CH1_BATTERY=absent %.3fV %+.3fA usb=%s INA_CH2_ESP32_5V=%.3fV %+.3fA INA_CH3_AD7606_5V=%.3fV %+.3fA",
        ina3221_policy_state_name(
            decision->state
        ),
        channels[0].bus_voltage_v,
        channels[0].current_a,
        decision->usb_present ? "yes" : "no",
        channels[1].bus_voltage_v,
        channels[1].current_a,
        channels[2].bus_voltage_v,
        channels[2].current_a
    );
}


static void ina3221_monitor_wait(void)
{
    (void)ulTaskNotifyTake(
        pdTRUE,
        pdMS_TO_TICKS(
            INA3221_MONITOR_PERIOD_MS
        )
    );
}


static void ina3221_monitor_worker(
    void *context
)
{
    (void)context;

    ina3221_driver_t driver = { 0 };
    ina3221_power_classifier_t classifier = { 0 };

    uint32_t sample_count = 0;
    uint32_t retry_count = 0;

    bool hardware_available =
        false;

    while (s_running) {
        if (!driver.ready) {
            const esp_err_t open_err =
                ina3221_driver_open(
                    &driver
                );

            if (open_err != ESP_OK) {
                ina3221_monitor_publish_invalid();

                retry_count++;

                if (
                    retry_count == 1 ||
                    retry_count %
                        INA3221_RETRY_LOG_INTERVAL == 0
                ) {
                    ESP_LOGW(
                        TAG,
                        "INA3221 unavailable at 0x40; retrying: %s",
                        esp_err_to_name(
                            open_err
                        )
                    );
                }

                hardware_available =
                    false;

                ina3221_monitor_wait();
                continue;
            }

            retry_count =
                0;

            memset(
                &classifier,
                0,
                sizeof(classifier)
            );

            if (!hardware_available) {
                ESP_LOGI(
                    TAG,
                    "INA3221 online: I2C 0x40, 100 kHz, avg=16, conversion=1.1 ms, period=1 s"
                );
            }

            hardware_available =
                true;
        }

        ina3221_channel_sample_t
            channels[INA3221_POLICY_CHANNEL_COUNT] = { 0 };

        const esp_err_t read_err =
            ina3221_driver_read_channels(
                &driver,
                channels
            );

        if (read_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "INA3221 read failed; measurements invalidated: %s",
                esp_err_to_name(
                    read_err
                )
            );

            ina3221_monitor_publish_invalid();

            ina3221_driver_close(
                &driver
            );

            memset(
                &classifier,
                0,
                sizeof(classifier)
            );

            hardware_available =
                false;

            ina3221_monitor_wait();
            continue;
        }

        const ina3221_power_state_t instantaneous_state =
            ina3221_policy_classify_instant(
                channels
            );

        const ina3221_power_state_t stable_state =
            ina3221_policy_classifier_update(
                &classifier,
                instantaneous_state
            );

        ina3221_power_decision_t decision;

        ina3221_policy_make_decision(
            channels,
            stable_state,
            &decision
        );

        ina3221_monitor_publish(
            channels,
            &decision
        );

        sample_count++;

        if (
            sample_count == 1 ||
            sample_count %
                INA3221_MONITOR_LOG_INTERVAL == 0
        ) {
            ina3221_monitor_log_sample(
                channels,
                &decision
            );
        }

        ina3221_monitor_wait();
    }

    ina3221_driver_close(
        &driver
    );

    s_task =
        NULL;

    vTaskDelete(
        NULL
    );
}


esp_err_t ina3221_monitor_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_running =
        true;

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            ina3221_monitor_worker,
            "ina3221",
            APP_TASK_STACK_INA3221,
            NULL,
            APP_TASK_PRIORITY_INA3221,
            &s_task,
            APP_TASK_CORE_SERVICES
        );

    if (result != pdPASS) {
        s_task =
            NULL;

        s_running =
            false;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Power monitor started on core=%d",
        APP_TASK_CORE_SERVICES
    );

    return ESP_OK;
}


void ina3221_monitor_stop(void)
{
    if (s_task == NULL) {
        s_running =
            false;

        return;
    }

    s_running =
        false;

    xTaskNotifyGive(
        s_task
    );

    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
}
