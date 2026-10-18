#include "app_test_wdt_policy.h"

#include "esp_system.h"

bool app_test_wdt_should_trigger(bool enabled, int reason)
{
    return enabled && reason == (int)ESP_RST_POWERON;
}
