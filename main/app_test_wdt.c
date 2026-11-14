#include "sdkconfig.h"

#if CONFIG_APP_TEST_WDT_TRIGGER

#include "app_test_wdt.h"

#include "app_startup_reset_reason.h"
#include "app_test_wdt_policy.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Was 2048: the test task had 72 bytes free and overflowed on the second boot (PANIC reset) in ESP-EMU. */
#define APP_TEST_WDT_TASK_STACK_SIZE 4096U

static const char *TAG = "TEST_WDT";

static void app_test_wdt_task(void *argument)
{
    (void)argument;

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    if (!app_test_wdt_should_trigger(
            CONFIG_APP_TEST_WDT_TRIGGER,
            (int)reset_reason)) {
        ESP_LOGI(
            TAG,
            "TEST_WDT: skipped, reset reason is not POWERON (%s)",
            app_startup_reset_reason_name(reset_reason)
        );
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(
        TAG,
        "TEST_WDT: arming interrupt-watchdog test in %d s",
        CONFIG_APP_TEST_WDT_DELAY_S
    );
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_TEST_WDT_DELAY_S * 1000U));

    ESP_LOGI(TAG, "TEST_WDT: triggering interrupt watchdog now");
    (void)fflush(stdout);

    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void app_test_wdt_start(void)
{
    const BaseType_t created = xTaskCreatePinnedToCore(
        app_test_wdt_task,
        "test_wdt",
        APP_TEST_WDT_TASK_STACK_SIZE,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        0
    );
    if (created != pdPASS) {
        ESP_LOGE(TAG, "TEST_WDT: failed to create interrupt-watchdog test task");
    }
}

#endif
