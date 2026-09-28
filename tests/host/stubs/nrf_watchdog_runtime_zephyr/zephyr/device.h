#ifndef ZEPHYR_DEVICE_H
#define ZEPHYR_DEVICE_H

struct device {
    int ready;
};

extern struct device s_test_watchdog_device;
static inline int device_is_ready(const struct device *device)
{
    return device != 0 && device->ready;
}

#define DEVICE_DT_GET(node_id) (&s_test_watchdog_device)

#endif
