#include "nrf_bme280_zephyr.h"

#include <stdatomic.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <stdio.h>
#include <zephyr/sys/printk.h>
#endif

#include "nrf_sensor_bus_zephyr.h"

static atomic_int s_state = -1;

static void bme280_observe_state(void *context, nrf_bme280_state_t state)
{
    (void)context;
    atomic_store(&s_state, (int)state);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    static const char *const names[] = {"PROBE", "CONFIGURE", "VERIFY", "MEASURING", "RECOVER"};
    printk("sensor_state: BME/BMP280 %s\n", names[state]);
#endif
}

int nrf_bme280_zephyr_state(void) { return atomic_load(&s_state); }
void nrf_bme280_zephyr_note_recover(void) { bme280_observe_state(NULL, NRF_BME280_RECOVER); }

static void bme280_zephyr_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    k_msleep((int32_t)delay_ms);
}

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
static void bme280_probe_text(const nrf_bme280_probe_t *probe, char *out, size_t size)
{
    if (!probe->acked) {
        (void)snprintf(out, size, "0x%02x NO ANSWER", (unsigned)probe->address);
    } else {
        const char *model = probe->chip_id == NRF_BME280_CHIP_ID ? "BME280"
            : (probe->chip_id == NRF_BMP280_CHIP_ID || probe->chip_id == NRF_BMP280_CHIP_ID_SAMPLE_A ||
               probe->chip_id == NRF_BMP280_CHIP_ID_SAMPLE_B) ? "BMP280" : "unknown part";
        (void)snprintf(out, size, "0x%02x answers, chip_id=0x%02x (%s)", (unsigned)probe->address,
                       (unsigned)probe->chip_id, model);
    }
}

/* One line per attempt (the init is retried every few seconds until it works). */
static void bme280_report(const nrf_bme280_t *device, nrf_bme280_result_t result)
{
    char first[64];
    char second[64];
    bme280_probe_text(&device->probe[0], first, sizeof(first));
    bme280_probe_text(&device->probe[1], second, sizeof(second));
    printk("sensor_probe: BME/BMP280 result=%d | %s | %s | used=%s\n", (int)result, first, second,
           result != NRF_BME280_OK ? "none" : (device->has_humidity ? "BME280 (humidity)" : "BMP280 (no humidity)"));
}
#endif

nrf_bme280_result_t nrf_bme280_zephyr_begin_remote(nrf_bme280_t *device)
{
    nrf_sensor_bus_t bus = {0};
    if (device == NULL) return NRF_BME280_ERR_ARGUMENT;
    if (!nrf_sensor_bus_zephyr_init_private(&bus)) {
        device->initialized = false;
        device->state = NRF_BME280_RECOVER;
        nrf_bme280_zephyr_note_recover();
        return NRF_BME280_ERR_BUS;
    }
    return nrf_bme280_begin_async(device, &bus, bme280_observe_state, NULL);
}

nrf_bme280_result_t nrf_bme280_zephyr_step(nrf_bme280_t *device, uint64_t now_ms)
{
    const nrf_bme280_result_t result = nrf_bme280_step_async(device, now_ms);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    if (result != NRF_BME280_PENDING && device != NULL) bme280_report(device, result);
#endif
    return result;
}

nrf_bme280_result_t nrf_bme280_zephyr_init_remote(nrf_bme280_t *device)
{
    nrf_sensor_bus_t bus = {0};
    if (device == NULL) {
        return NRF_BME280_ERR_ARGUMENT;
    }
    if (!nrf_sensor_bus_zephyr_init_private(&bus)) {
        device->initialized = false;
        device->state = NRF_BME280_RECOVER;
        nrf_bme280_zephyr_note_recover();
        return NRF_BME280_ERR_BUS;
    }

    const nrf_bme280_result_t result = nrf_bme280_init_observed(
        device,
        &bus,
        bme280_zephyr_delay_ms,
        NULL,
        bme280_observe_state,
        NULL);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    bme280_report(device, result);
#endif
    return result;
}
