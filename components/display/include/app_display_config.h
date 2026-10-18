#pragma once

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"

#if defined(CONFIG_APP_DISPLAY_ENABLED) && CONFIG_APP_DISPLAY_ENABLED
#define APP_DISPLAY_ENABLED 1
#else
#define APP_DISPLAY_ENABLED 0
#endif
#else
/* Native host tests have no generated sdkconfig; mirror the production default. */
#define APP_DISPLAY_ENABLED 1
#endif
