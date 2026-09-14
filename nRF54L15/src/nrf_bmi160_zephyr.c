#include "nrf_bmi160_zephyr.h"

#include <zephyr/kernel.h>

#include "nrf_sensor_bus_zephyr.h"

static void nrf_bmi160_zephyr_delay_ms(
    void *context,
    uint32_t delay_ms)
{
    (void)context;
    k_msleep((int32_t)delay_ms);
}

nrf_bmi160_result_t nrf_bmi160_zephyr_init_remote(nrf_bmi160_t *device)
{
    nrf_sensor_bus_t bus = {0};
    if (device == NULL) {
        return NRF_BMI160_ERR_ARGUMENT;
    }
    if (!nrf_sensor_bus_zephyr_init_private(&bus)) {
        return NRF_BMI160_ERR_BUS;
    }

    return nrf_bmi160_init(
        device,
        &bus,
        NRF_BMI160_DEFAULT_ADDRESS,
        nrf_bmi160_zephyr_delay_ms,
        NULL);
}
