#include "nrf_sensor_bus_zephyr.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <zephyr/sys/printk.h>
#endif

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
#if SIZE_MAX > UINT32_MAX
    if (write_len > UINT32_MAX || read_len > UINT32_MAX) {
        return -EINVAL;
    }
#endif

    int result;
    if (write_len > 0U && read_len > 0U) {
        result = i2c_write_read(
            zephyr_context->device,
            address,
            write_data,
            write_len,
            read_data,
            read_len);
    } else if (write_len > 0U) {
        result = i2c_write(
            zephyr_context->device,
            write_data,
            (uint32_t)write_len,
            address);
    } else if (read_len > 0U) {
        result = i2c_read(
            zephyr_context->device,
            read_data,
            (uint32_t)read_len,
            address);
    } else {
        result = -EINVAL;
    }

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    if (result != 0) {
        printk("sensor_i2c: bus=i2c30 addr=0x%02x reg=%d write=%u read=%u ret=%d\n",
               (unsigned)address,
               write_data != NULL && write_len > 0U ? (int)write_data[0] : -1,
               (unsigned)write_len, (unsigned)read_len, result);
    }
#endif
    return result;
}

#define NRF_SENSOR_BUS_SCAN_FIRST_ADDRESS 0x08U
#define NRF_SENSOR_BUS_SCAN_LAST_ADDRESS 0x77U

int nrf_sensor_bus_zephyr_scan_private(void)
{
    static const uint8_t id_registers[] = {0x00U, 0x75U, 0xD0U};
    int found = 0;

    if (!device_is_ready(s_private_context.device)) {
        return -ENODEV;
    }
    for (uint16_t address = NRF_SENSOR_BUS_SCAN_FIRST_ADDRESS;
         address <= NRF_SENSOR_BUS_SCAN_LAST_ADDRESS; ++address) {
        uint8_t byte = 0U;
        if (i2c_read(s_private_context.device, &byte, 1U, address) != 0) {
            continue;
        }
        ++found;
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        int ids[sizeof(id_registers)];
        for (size_t i = 0U; i < sizeof(id_registers); ++i) {
            uint8_t value = 0U;
            ids[i] = i2c_write_read(s_private_context.device, address, &id_registers[i], 1U, &value, 1U) == 0
                ? (int)value
                : -1;
        }
        printk("sensor_scan: bus=i2c30 addr=0x%02x ack reg00=%d reg75=%d regD0=%d (-1 = no answer)\n",
               (unsigned)address, ids[0], ids[1], ids[2]);
#else
        (void)id_registers;
#endif
    }
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    printk("sensor_scan: bus=i2c30 devices=%d\n", found);
#endif
    return found;
}

bool nrf_sensor_bus_zephyr_init_private(nrf_sensor_bus_t *out_bus)
{
    if (out_bus == NULL) {
        return false;
    }

    *out_bus = (nrf_sensor_bus_t){0};
    if (!device_is_ready(s_private_context.device)) {
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        printk("sensor_i2c: bus=i2c30 ready=0\n");
#endif
        return false;
    }

    out_bus->transfer = nrf_sensor_bus_zephyr_transfer;
    out_bus->context = &s_private_context;
    return true;
}
