#ifndef ZEPHYR_DRIVERS_WATCHDOG_H
#define ZEPHYR_DRIVERS_WATCHDOG_H

#include <stdint.h>

struct device;
struct wdt_timeout_cfg {
    struct {
        uint32_t min;
        uint32_t max;
    } window;
    void (*callback)(struct device *, int);
    uint8_t flags;
};

#define WDT_FLAG_RESET_SOC 0x01U
#define WDT_OPT_PAUSE_HALTED_BY_DBG 0x01U

int wdt_install_timeout(const struct device *device, const struct wdt_timeout_cfg *config);
int wdt_setup(const struct device *device, uint8_t options);
int wdt_feed(const struct device *device, int channel_id);

#endif
