#include "app_test_wdt_policy.h"

#include <assert.h>

#include "esp_system.h"

int main(void)
{
    assert(app_test_wdt_should_trigger(true, ESP_RST_POWERON));
    assert(!app_test_wdt_should_trigger(true, ESP_RST_INT_WDT));
    assert(!app_test_wdt_should_trigger(true, ESP_RST_PANIC));
    assert(!app_test_wdt_should_trigger(true, ESP_RST_SW));
    assert(!app_test_wdt_should_trigger(false, ESP_RST_POWERON));
    return 0;
}
