#include "nrf_bme280_zephyr.h"

#include <zephyr/kernel.h>

#include "nrf_sensor_bus_zephyr.h"

static void bme280_zephyr_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    k_msleep((int32_t)delay_ms);
}

bool nrf_bme280_zephyr_init_remote(nrf_bme280_t *device)
{
    nrf_sensor_bus_t bus;
    if (device == NULL || !nrf_sensor_bus_zephyr_init_private(&bus)) {
        return false;
    }

    return nrf_bme280_init(
               device,
               &bus,
               bme280_zephyr_delay_ms,
               NULL) == NRF_BME280_OK;
}
