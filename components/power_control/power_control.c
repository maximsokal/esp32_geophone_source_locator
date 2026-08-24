#include "power_control.h"

#include <stdbool.h>

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_attr.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "POWER_CONTROL";


#define BUTTON_POLL_MS                  10
#define BUTTON_DEBOUNCE_MS              50
#define BUTTON_FALLBACK_SAMPLES          2

#define OFF_PULSE_MS                   200

#define POWER_MONITOR_TASK_STACK_SIZE  3072
#define POWER_MONITOR_TASK_PRIORITY       5


static volatile bool s_shutdown_latched =
    false;


static volatile bool s_monitor_running =
    false;


static volatile bool s_button_armed =
    false;


static TaskHandle_t s_shutdown_wait_task =
    NULL;


static TaskHandle_t s_monitor_task =
    NULL;


static bool s_isr_handler_ready =
    false;


static void power_control_notify_waiter_from_task(void)
{
    TaskHandle_t waiting_task =
        s_shutdown_wait_task;

    if (waiting_task != NULL) {
        xTaskNotifyGive(
            waiting_task
        );
    }
}


static void power_control_latch_from_task(void)
{
    if (s_shutdown_latched) {
        return;
    }

    s_shutdown_latched =
        true;

    ESP_LOGI(
        TAG,
        "Shutdown request latched by button monitor"
    );

    power_control_notify_waiter_from_task();
}


static void IRAM_ATTR power_button_isr(
    void *arg
)
{
    (void)arg;

    if (s_shutdown_latched) {
        return;
    }

    s_shutdown_latched =
        true;

    TaskHandle_t waiting_task =
        s_shutdown_wait_task;

    if (waiting_task == NULL) {
        return;
    }

    BaseType_t higher_priority_task_woken =
        pdFALSE;

    vTaskNotifyGiveFromISR(
        waiting_task,
        &higher_priority_task_woken
    );

    if (
        higher_priority_task_woken ==
        pdTRUE
    ) {
        portYIELD_FROM_ISR();
    }
}


static void wait_for_button_release(void)
{
    while (
        gpio_get_level(
            BOARD_POWER_BUTTON_GPIO
        ) == 1
    ) {
        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            BUTTON_DEBOUNCE_MS
        )
    );
}


static void power_button_monitor_task(
    void *argument
)
{
    (void)argument;

    /*
     * The same physical button powers the device on.
     * Ignore that initial press completely.
     */
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

    while (s_monitor_running) {
        while (
            s_monitor_running &&
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 1
        ) {
            vTaskDelay(
                pdMS_TO_TICKS(
                    BUTTON_POLL_MS
                )
            );
        }

        if (!s_monitor_running) {
            break;
        }

        /*
         * Require the released state to remain stable before
         * the power button becomes a shutdown input.
         */
        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_DEBOUNCE_MS
            )
        );

        if (
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 0
        ) {
            break;
        }
    }

    if (!s_monitor_running) {
        s_monitor_task =
            NULL;

        vTaskDelete(
            NULL
        );

        return;
    }

    /*
     * Arm the hardware interrupt after the startup press
     * has been released.
     */
    if (s_isr_handler_ready) {
        esp_err_t err =
            gpio_set_intr_type(
                BOARD_POWER_BUTTON_GPIO,
                GPIO_INTR_POSEDGE
            );

        if (err == ESP_OK) {
            err =
                gpio_intr_enable(
                    BOARD_POWER_BUTTON_GPIO
                );
        }

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "GPIO interrupt unavailable, polling fallback remains active: %s",
                esp_err_to_name(err)
            );
        }
    }

    s_button_armed =
        true;

    ESP_LOGI(
        TAG,
        "Power button armed"
    );

    unsigned high_samples =
        0;

    /*
     * Polling is deliberately kept in addition to the ISR.
     *
     * Long SD operations may occasionally make edge-based
     * detection less reliable. A held button therefore has
     * a second independent path to latch shutdown.
     */
    while (
        s_monitor_running &&
        !s_shutdown_latched
    ) {
        if (
            gpio_get_level(
                BOARD_POWER_BUTTON_GPIO
            ) == 1
        ) {
            high_samples++;

            if (
                high_samples >=
                BUTTON_FALLBACK_SAMPLES
            ) {
                power_control_latch_from_task();
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

    if (s_isr_handler_ready) {
        gpio_intr_disable(
            BOARD_POWER_BUTTON_GPIO
        );
    }

    s_button_armed =
        false;

    s_monitor_task =
        NULL;

    vTaskDelete(
        NULL
    );
}


esp_err_t power_control_init(void)
{
    const gpio_config_t off_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_POWER_OFF_GPIO,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &off_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure OFF GPIO: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        gpio_set_level(
            BOARD_POWER_OFF_GPIO,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set OFF GPIO LOW: %s",
            esp_err_to_name(err)
        );

        return err;
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
         * The monitor task arms the interrupt only after the
         * startup power-button press has been released.
         */
        .intr_type =
            GPIO_INTR_DISABLE,
    };

    err =
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

    err =
        gpio_install_isr_service(
            0
        );

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        ESP_LOGE(
            TAG,
            "Failed to install GPIO ISR service: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        gpio_isr_handler_add(
            BOARD_POWER_BUTTON_GPIO,
            power_button_isr,
            NULL
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to add power button ISR: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_isr_handler_ready =
        true;

    s_shutdown_latched =
        false;

    s_shutdown_wait_task =
        NULL;

    s_button_armed =
        false;

    s_monitor_running =
        true;

    BaseType_t task_result =
        xTaskCreate(
            power_button_monitor_task,
            "power_button",
            POWER_MONITOR_TASK_STACK_SIZE,
            NULL,
            POWER_MONITOR_TASK_PRIORITY,
            &s_monitor_task
        );

    if (task_result != pdPASS) {
        s_monitor_running =
            false;

        s_monitor_task =
            NULL;

        ESP_LOGE(
            TAG,
            "Failed to create power button monitor"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Initialized: OFF=GPIO%d BUTTON=GPIO%d",
        BOARD_POWER_OFF_GPIO,
        BOARD_POWER_BUTTON_GPIO
    );

    return ESP_OK;
}


void power_control_wait_for_shutdown_request(void)
{
    s_shutdown_wait_task =
        xTaskGetCurrentTaskHandle();

    /*
     * Remove stale task notification counts.
     *
     * Do not clear s_shutdown_latched here. A request may
     * already have been captured while the application was
     * still starting or while another worker was busy.
     */
    while (
        ulTaskNotifyTake(
            pdTRUE,
            0
        ) > 0
    ) {
    }

    while (!s_shutdown_latched) {
        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );
    }

    if (s_isr_handler_ready) {
        gpio_intr_disable(
            BOARD_POWER_BUTTON_GPIO
        );
    }

    s_monitor_running =
        false;

    ESP_LOGI(
        TAG,
        "Shutdown button latched"
    );

    /*
     * The request remains latched even after the physical
     * button is released.
     */
    wait_for_button_release();

    s_shutdown_wait_task =
        NULL;

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );
}


bool power_control_is_shutdown_requested(void)
{
    return s_shutdown_latched;
}


void power_control_shutdown(void)
{
    s_monitor_running =
        false;

    s_button_armed =
        false;

    if (s_isr_handler_ready) {
        gpio_intr_disable(
            BOARD_POWER_BUTTON_GPIO
        );
    }

    ESP_LOGW(
        TAG,
        "Sending OFF pulse to Pololu"
    );

    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            OFF_PULSE_MS
        )
    );

    /*
     * With Pololu-only power the ESP32 should already
     * be physically off before reaching this point.
     *
     * If USB still powers the ESP32, return OFF LOW.
     */
    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        0
    );

    ESP_LOGW(
        TAG,
        "ESP32 still powered - probably USB power is connected"
    );
}