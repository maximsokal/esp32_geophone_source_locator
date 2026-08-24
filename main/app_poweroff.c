#include "app_poweroff.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


void app_poweroff_idle_forever(void)
{
    /*
     * Pololu removes project power in normal standalone use.
     *
     * During USB debugging the ESP32 can remain electrically
     * powered after Pololu turns the project rail off. Keep the
     * main task alive in a safe idle state instead of returning.
     */
    for (;;) {
        vTaskDelay(
            pdMS_TO_TICKS(
                1000
            )
        );
    }
}
