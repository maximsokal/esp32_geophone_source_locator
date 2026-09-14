#include "nrf_sensor_bus_zephyr.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>

typedef struct {
    const struct device *device;
} nrf_sensor_bus_zephyr_context_t;

static nrf_sensor_bus_zephyr_context_t s_private_context = {
    .device = DEVICE_DT_GET(DT_NODELABEL(i2c30)),
};

static int nrf_sensor_bus_zephyr_transfer(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    nrf_sensor_bus_zephyr_context_t *zephyr_context =
        (nrf_sensor_bus_zephyr_context_t *)context;
    if (zephyr_context == NULL || zephyr_context->device == NULL) {
        return -ENODEV;
    }

    if (write_len > 0U && read_len > 0U) {
        return i2c_write_read(
            zephyr_context->device,
            address,
            write_data,
            write_len,
            read_data,
            read_len);
    }
    if (write_len > 0U) {
        return i2c_write(
            zephyr_context->device,
            write_data,
            write_len,
            address);
    }
    if (read_len > 0U) {
        return i2c_read(
            zephyr_context->device,
            read_data,
            read_len,
            address);
    }

    return -EINVAL;
}

bool nrf_sensor_bus_zephyr_init_private(nrf_sensor_bus_t *out_bus)
{
    if (out_bus == NULL) {
        return false;
    }

    *out_bus = (nrf_sensor_bus_t){0};
    if (!device_is_ready(s_private_context.device)) {
        return false;
    }

    out_bus->transfer = nrf_sensor_bus_zephyr_transfer;
    out_bus->context = &s_private_context;
    return true;
}
