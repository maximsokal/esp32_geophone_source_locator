#pragma once

#include <stdbool.h>

/* reason uses the ESP_RST_* value for host-friendly policy evaluation. */
bool app_test_wdt_should_trigger(bool enabled, int reason);
