#pragma once
#include <stdint.h>

/* Host-test controllable clock: the .c test file defines this and a
 * setter, so a test can simulate the calculation time budget expiring
 * without any real sleeping. */
int64_t esp_timer_get_time(void);
