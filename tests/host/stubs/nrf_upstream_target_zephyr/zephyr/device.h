#ifndef ZEPHYR_DEVICE_H
#define ZEPHYR_DEVICE_H

#include <stdbool.h>

struct device {
    int placeholder;
};

extern const struct device nrf_upstream_target_test_i2c22_device;
bool device_is_ready(const struct device *device);

#define DEVICE_DT_GET(node_id) (&nrf_upstream_target_test_i2c22_device)

#endif
