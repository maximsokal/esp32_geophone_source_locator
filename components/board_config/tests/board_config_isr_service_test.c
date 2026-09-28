#include <assert.h>

#include "board_config.h"
#include "esp_intr_alloc.h"


static unsigned s_install_calls;
static int s_install_flags;
static esp_err_t s_install_result = ESP_OK;


esp_err_t gpio_install_isr_service(int flags)
{
    ++s_install_calls;
    s_install_flags = flags;
    return s_install_result;
}


int main(void)
{
    s_install_result = ESP_FAIL;
    assert(board_gpio_isr_service_install() == ESP_FAIL);
    assert(s_install_calls == 1U);
    assert(s_install_flags == ESP_INTR_FLAG_IRAM);

    /* A prior compatible installation is indistinguishable from any other
     * ESP_ERR_INVALID_STATE in IDF; the sole-owner contract documents this. */
    s_install_result = ESP_ERR_INVALID_STATE;
    assert(board_gpio_isr_service_install() == ESP_OK);
    assert(s_install_calls == 2U);
    assert(s_install_flags == ESP_INTR_FLAG_IRAM);

    s_install_result = ESP_FAIL;
    assert(board_gpio_isr_service_install() == ESP_OK);
    assert(s_install_calls == 2U);
    return 0;
}
