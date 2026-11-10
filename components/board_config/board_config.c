#include "board_config.h"

#include <stdbool.h>

#include "esp_intr_alloc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define BOARD_GPIO_ISR_INSTALL_STACK_BYTES 3072U
#define BOARD_GPIO_ISR_INSTALL_PRIORITY    5U
#define BOARD_GPIO_ISR_INSTALL_TIMEOUT_MS  2000U

typedef struct
{
    TaskHandle_t caller;
    esp_err_t result;
} board_gpio_isr_install_t;

static bool s_gpio_isr_service_ready;
/* Static: the helper task may outlive a caller that gave up waiting for it. */
static board_gpio_isr_install_t s_gpio_isr_install;


static void board_gpio_isr_install_task(void *argument)
{
    board_gpio_isr_install_t *install = (board_gpio_isr_install_t *)argument;
    install->result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    xTaskNotifyGive(install->caller);
    vTaskDelete(NULL);
}


/*
 * ESP-IDF binds the GPIO interrupt, and with it every handler added later, to
 * the core that calls gpio_install_isr_service(). The call cannot be forwarded
 * with esp_ipc_call_blocking(): the installer itself makes a blocking IPC call
 * to its own core, which deadlocks inside the IPC task. A short-lived task
 * pinned to the target core is the supported way to choose the core.
 */
static esp_err_t board_gpio_isr_install_on_isr_core(void)
{
    if (xPortGetCoreID() == BOARD_GPIO_ISR_CORE) {
        return gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    }

    s_gpio_isr_install.caller = xTaskGetCurrentTaskHandle();
    s_gpio_isr_install.result = ESP_FAIL;
    if (xTaskCreatePinnedToCore(
            board_gpio_isr_install_task,
            "gpio_isr_inst",
            BOARD_GPIO_ISR_INSTALL_STACK_BYTES,
            &s_gpio_isr_install,
            BOARD_GPIO_ISR_INSTALL_PRIORITY,
            NULL,
            BOARD_GPIO_ISR_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BOARD_GPIO_ISR_INSTALL_TIMEOUT_MS)) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return s_gpio_isr_install.result;
}


esp_err_t board_gpio_isr_service_install(void)
{
    if (s_gpio_isr_service_ready) {
        return ESP_OK;
    }

    const esp_err_t err = board_gpio_isr_install_on_isr_core();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE means somebody installed it first; IDF exposes no way
         * to verify that service's flags, so see the sole-owner contract. */
        s_gpio_isr_service_ready = true;
        return ESP_OK;
    }

    return err;
}
