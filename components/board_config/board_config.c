#include "board_config.h"

#include <stdbool.h>

#include "esp_intr_alloc.h"


static bool s_gpio_isr_service_ready;


esp_err_t board_gpio_isr_service_install(void)
{
    if (s_gpio_isr_service_ready) {
        return ESP_OK;
    }

    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE means somebody installed it first; IDF exposes no way
         * to verify that service's flags, so see the sole-owner contract. */
        s_gpio_isr_service_ready = true;
        return ESP_OK;
    }

    return err;
}
