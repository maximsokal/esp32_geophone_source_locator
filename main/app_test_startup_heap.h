#pragma once

#include <stdbool.h>

/* Emulator-only: hold internal heap pressure until explicitly released. */
bool app_test_startup_heap_prepare(void);
void app_test_startup_heap_release(void);
