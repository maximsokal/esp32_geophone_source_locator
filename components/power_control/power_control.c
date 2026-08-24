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


#define BUTTON_POLL_MS       10
#define BUTTON_DEBOUNCE_MS   50
#define OFF_PULSE_MS         200


static volatile bool s_shutdown_latched =
    false;


static TaskHandle_t s_shutdown_wait_task =
    NULL;


static bool s_isr_handler_ready =
    false;


static void IRAM_ATTR power_button_isr(
    void *arg
)
{
    (void)arg;


    if (
        s_shutdown_latched
    ) {
        return;
    }


    s_shutdown_latched =
        true;


    TaskHandle_t waiting_task =
        s_shutdown_wait_task;


    if (
        waiting_task ==
        NULL
    ) {
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


    if (
        err !=
        ESP_OK
    ) {
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


    if (
        err !=
        ESP_OK
    ) {
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
         * Interrupt remains disabled until the startup
         * power-button press has been released.
         */
        .intr_type =
            GPIO_INTR_DISABLE,
    };


    err =
        gpio_config(
            &button_config
        );


    if (
        err !=
        ESP_OK
    ) {
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


    if (
        err !=
        ESP_OK
    ) {
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


    ESP_LOGI(
        TAG,
        "Initialized: OFF=GPIO%d BUTTON=GPIO%d",
        BOARD_POWER_OFF_GPIO,
        BOARD_POWER_BUTTON_GPIO
    );


    return ESP_OK;
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


void power_control_wait_for_shutdown_request(void)
{
    /*
     * The device may boot while the same button used
     * to power it on is still being held.
     *
     * Do not interpret that initial press as shutdown.
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


        wait_for_button_release();
    }


    s_shutdown_latched =
        false;


    s_shutdown_wait_task =
        xTaskGetCurrentTaskHandle();


    /*
     * Remove stale task notification counts before
     * the shutdown interrupt is armed.
     */
    while (
        ulTaskNotifyTake(
            pdTRUE,
            0
        ) > 0
    ) {
    }


    if (
        !s_isr_handler_ready
    ) {
        ESP_LOGE(
            TAG,
            "Power button ISR is not available"
        );


        return;
    }


    esp_err_t err =
        gpio_set_intr_type(
            BOARD_POWER_BUTTON_GPIO,
            GPIO_INTR_POSEDGE
        );


    if (
        err !=
        ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "Failed to set power button interrupt: %s",
            esp_err_to_name(err)
        );


        return;
    }


    err =
        gpio_intr_enable(
            BOARD_POWER_BUTTON_GPIO
        );


    if (
        err !=
        ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "Failed to enable power button interrupt: %s",
            esp_err_to_name(err)
        );


        return;
    }


    /*
     * Cover the tiny race where the button becomes HIGH
     * immediately before the interrupt is enabled.
     */
    if (
        gpio_get_level(
            BOARD_POWER_BUTTON_GPIO
        ) == 1
    ) {
        s_shutdown_latched =
            true;


        xTaskNotifyGive(
            s_shutdown_wait_task
        );
    }


    ESP_LOGI(
        TAG,
        "Power button armed"
    );


    while (
        !s_shutdown_latched
    ) {
        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );
    }


    gpio_intr_disable(
        BOARD_POWER_BUTTON_GPIO
    );


    ESP_LOGI(
        TAG,
        "Shutdown button latched"
    );


    /*
     * The request is already stored.
     * It remains valid even if the button is released
     * while another task is still finishing SD work.
     */
    wait_for_button_release();


    s_shutdown_wait_task =
        NULL;


    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );
}


void power_control_shutdown(void)
{
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