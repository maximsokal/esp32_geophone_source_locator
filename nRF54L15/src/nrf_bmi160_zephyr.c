#include "nrf_bmi160_zephyr.h"

#include <zephyr/kernel.h>
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <zephyr/sys/printk.h>
#endif

#include "nrf_sensor_bus_zephyr.h"

static void nrf_bmi160_zephyr_delay_ms(
    void *context,
    uint32_t delay_ms)
{
    (void)context;
    k_msleep((int32_t)delay_ms);
}

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
static const char *bmi160_result_text(nrf_bmi160_result_t result)
{
    switch (result) {
    case NRF_BMI160_OK: return "ok";
    case NRF_BMI160_ERR_BUS: return "no answer / transfer failed";
    case NRF_BMI160_ERR_CHIP_ID: return "answers, but the chip id is not a BMI160";
    case NRF_BMI160_ERR_PMU: return "found, power state did not come up";
    case NRF_BMI160_ERR_CONFIG: return "found, configuration did not stick";
    default: return "other error";
    }
}

/* One line per address and attempt (the init is retried every few seconds until it works). */
static void bmi160_report(uint8_t address, nrf_bmi160_result_t result, uint8_t chip_id)
{
    if (result == NRF_BMI160_ERR_BUS) {
        printk("sensor_probe: BMI160 addr=0x%02x NO ANSWER (%s)\n", (unsigned)address, bmi160_result_text(result));
    } else {
        printk("sensor_probe: BMI160 addr=0x%02x result=%d (%s) chip_id=0x%02x (BMI160 = 0xd1)\n",
               (unsigned)address, (int)result, bmi160_result_text(result), (unsigned)chip_id);
    }
}
#endif

nrf_bmi160_result_t nrf_bmi160_zephyr_init_remote(nrf_bmi160_t *device)
{
    static const uint8_t addresses[] = {NRF_BMI160_DEFAULT_ADDRESS, NRF_BMI160_ALTERNATE_ADDRESS};
    nrf_sensor_bus_t bus = {0};
    nrf_bmi160_result_t best = NRF_BMI160_ERR_BUS;
    nrf_bmi160_t attempt = {0};

    if (device == NULL) {
        return NRF_BMI160_ERR_ARGUMENT;
    }
    if (!nrf_sensor_bus_zephyr_init_private(&bus)) {
        return NRF_BMI160_ERR_BUS;
    }

    /* The GY-BMI160 sits on 0x68 (SA0 low) or 0x69 (SA0 high): try both and say what each one did. */
    for (size_t i = 0U; i < sizeof(addresses); ++i) {
        const nrf_bmi160_result_t result = nrf_bmi160_init(
            &attempt,
            &bus,
            addresses[i],
            nrf_bmi160_zephyr_delay_ms,
            NULL);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        bmi160_report(addresses[i], result, attempt.chip_id);
#endif
        if (result == NRF_BMI160_OK) {
            *device = attempt;
            return NRF_BMI160_OK;
        }
        /* Keep the most informative failure: an answering device beats silence. */
        if (i == 0U || best == NRF_BMI160_ERR_BUS) {
            best = result;
        }
    }

    *device = attempt;
    device->initialized = false;
    return best;
}
