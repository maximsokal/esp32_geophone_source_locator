#pragma once

#ifdef _MSC_VER
/* GNU attributes in the vendor public headers have no MSVC spelling. */
#define __attribute__(...)
#endif

/* Same Button 4.2.0 timing settings as the production sdkconfig. */
#define CONFIG_BUTTON_PERIOD_TIME_MS 10
#define CONFIG_BUTTON_DEBOUNCE_TICKS 6
#define CONFIG_BUTTON_SHORT_PRESS_TIME_MS 180
#define CONFIG_BUTTON_LONG_PRESS_TIME_MS 1500
#define CONFIG_BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS 20
#define BUTTON_VER_MAJOR 4
#define BUTTON_VER_MINOR 2
#define BUTTON_VER_PATCH 0
