#pragma once

/* Match the current espressif/button scan-period default used by the target
 * unless the application explicitly overrides it in sdkconfig. The production
 * source validates its timing relationship against the generated target value. */
#define CONFIG_BUTTON_PERIOD_TIME_MS 5
