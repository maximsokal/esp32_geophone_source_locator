#include "power_button_monitor.h"

#include <stdbool.h>

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"


static const char *TAG =
    "POWER_BUTTON";


#define BUTTON_POLL_MS                    10
#define BUTTON_PRESS_CONFIRM_MS          150
#define BUTTON_RELEASE_CONFIRM_MS         50

#define POWER_MONITOR_TASK_STACK_SIZE   3072
#define POWER_MONITOR_TASK_PRIORITY        5

#define POWER_EVENT_SHUTDOWN_REQUESTED  BIT0


static EventGroupHandle_t s_events =
    NULL;


static TaskHandle_t s_task =
    NULL;


static volatile bool s_monitor_running =
    false;


static bool s_initialized =
    false;


static unsigned required_samples(
    unsigned duration_ms
)
{
    unsigned samples =
        duration_ms /
        BUTTON_POLL_MS;

    if (
        duration_ms %
        BUTTON_POLL_MS != 0
    ) {
        samples++;
    }

    return
        samples > 0
            ? samples
            : 1;
}


static void wait_for_stable_release(void)
{
    const unsigned required_low_samples =
        required_samples(
            BUTTON_RELEASE_CONFIRM_MS
        );

    unsigned low_samples =
        0;

    while (
        low_samples <
        required_low_samples
    ) {
        if (
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 0
        ) {
            low_samples++;
        }
        else {
            low_samples =
                0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }
}


static bool wait_for_startup_release(void)
{
    const unsigned required_low_samples =
        required_samples(
            BUTTON_RELEASE_CONFIRM_MS
        );

    unsigned low_samples =
        0;

    if (
        gpio_get_level(
            BOARD_POWER_BUTTON_GPIO
        ) == 1
    ) {
        ESP_LOGI(
            TAG,
            "Waiting for startup button release"
        );
    }

    while (
        s_monitor_running &&
        low_samples <
            required_low_samples
    ) {
        if (
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 0
        ) {
            low_samples++;
        }
        else {
            low_samples =
                0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }

    return
        s_monitor_running;
}


static void power_button_monitor_task(
    void *argument
)
{
    (void)argument;

    if (
        !wait_for_startup_release()
    ) {
        s_task =
            NULL;

        vTaskDelete(
            NULL
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Power button armed"
    );

    const unsigned required_high_samples =
        required_samples(
            BUTTON_PRESS_CONFIRM_MS
        );

    unsigned high_samples =
        0;

    while (s_monitor_running) {
        if (
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 1
        ) {
            high_samples++;

            if (
                high_samples >=
                required_high_samples
            ) {
                xEventGroupSetBits(
                    s_events,
                    POWER_EVENT_SHUTDOWN_REQUESTED
                );

                ESP_LOGI(
                    TAG,
                    "Shutdown request confirmed after %u ms HIGH",
                    BUTTON_PRESS_CONFIRM_MS
                );

                break;
            }
        }
        else {
            high_samples =
                0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }

    s_monitor_running =
        false;

    s_task =
        NULL;

    vTaskDelete(
        NULL
    );
}


esp_err_t power_button_monitor_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const gpio_config_t button_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_POWER_BUTTON_GPIO,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        /*
         * Shutdown confirmation is level-based and debounced
         * in the monitor task. A GPIO edge alone must never
         * latch a shutdown request.
         */
        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &button_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure button GPIO: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_events =
        xEventGroupCreate();

    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xEventGroupClearBits(
        s_events,
        POWER_EVENT_SHUTDOWN_REQUESTED
    );

    s_monitor_running =
        true;

    BaseType_t task_result =
        xTaskCreate(
            power_button_monitor_task,
            "power_button",
            POWER_MONITOR_TASK_STACK_SIZE,
            NULL,
            POWER_MONITOR_TASK_PRIORITY,
            &s_task
        );

    if (task_result != pdPASS) {
        s_monitor_running =
            false;

        s_task =
            NULL;

        vEventGroupDelete(
            s_events
        );

        s_events =
            NULL;

        ESP_LOGE(
            TAG,
            "Failed to create power button monitor"
        );

        return ESP_ERR_NO_MEM;
    }

    s_initialized =
        true;

    return ESP_OK;
}


void power_button_monitor_wait_for_shutdown_request(void)
{
    if (
        !s_initialized ||
        s_events == NULL
    ) {
        return;
    }

    xEventGroupWaitBits(
        s_events,
        POWER_EVENT_SHUTDOWN_REQUESTED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(
        TAG,
        "Shutdown button latched"
    );

    /*
     * The same physical button drives Pololu ON.
     * Wait until it is stably released before the firmware
     * later pulses Pololu OFF.
     */
    wait_for_stable_release();

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );
}


bool power_button_monitor_is_shutdown_requested(void)
{
    if (
        !s_initialized ||
        s_events == NULL
    ) {
        return false;
    }

    EventBits_t bits =
        xEventGroupGetBits(
            s_events
        );

    return
        (
            bits &
            POWER_EVENT_SHUTDOWN_REQUESTED
        ) != 0;
}


void power_button_monitor_stop(void)
{
    if (!s_initialized) {
        return;
    }

    s_monitor_running =
        false;

    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }
}
