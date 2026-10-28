#include "nrf_bmi160_zephyr.h"

#include <stdatomic.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <zephyr/sys/printk.h>
#endif

#include "nrf_sensor_bus_zephyr.h"

static atomic_int s_state = -1;

static void bmi160_observe_state(void *context, nrf_bmi160_state_t state)
{
    (void)context;
    atomic_store(&s_state, (int)state);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    static const char *const names[] = {"PROBE", "CONFIGURE", "VERIFY", "MEASURING", "RECOVER"};
    printk("sensor_state: BMI160 %s\n", names[state]);
#endif
}

int nrf_bmi160_zephyr_state(void) { return atomic_load(&s_state); }
void nrf_bmi160_zephyr_note_recover(void) { bmi160_observe_state(NULL, NRF_BMI160_RECOVER); }

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
static void bmi160_report(uint8_t address, nrf_bmi160_result_t result, const nrf_bmi160_t *device)
{
    if (result == NRF_BMI160_ERR_BUS) {
        printk("sensor_probe: BMI160 addr=0x%02x NO ANSWER (%s)\n", (unsigned)address, bmi160_result_text(result));
        if (device->chip_id == 0U) nrf_sensor_bus_zephyr_diagnose_bmi_probe(address);
    } else {
        printk("sensor_probe: BMI160 addr=0x%02x result=%d (%s) chip_id=0x%02x (BMI160 = 0xd1)\n",
               (unsigned)address, (int)result, bmi160_result_text(result), (unsigned)device->chip_id);
    }
    if (device->err_reg_valid)
        printk("sensor_probe: BMI160 addr=0x%02x ERR_REG=0x%02x\n",
               (unsigned)address, (unsigned)device->err_reg);
}
#endif

nrf_bmi160_result_t nrf_bmi160_zephyr_begin_address(nrf_bmi160_t *device, uint8_t address)
{
    nrf_sensor_bus_t bus = {0};
    if (device == NULL) return NRF_BMI160_ERR_ARGUMENT;
    if (!nrf_sensor_bus_zephyr_init_private(&bus)) {
        device->initialized = false;
        device->state = NRF_BMI160_RECOVER;
        nrf_bmi160_zephyr_note_recover();
        return NRF_BMI160_ERR_BUS;
    }
    return nrf_bmi160_begin_async(device, &bus, address, bmi160_observe_state, NULL);
}

nrf_bmi160_result_t nrf_bmi160_zephyr_step(nrf_bmi160_t *device, uint64_t now_ms)
{
    const nrf_bmi160_result_t result = nrf_bmi160_step_async(device, now_ms);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    if (result != NRF_BMI160_PENDING && device != NULL)
        bmi160_report(device->address, result, device);
#endif
    return result;
}

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
        device->initialized = false;
        device->state = NRF_BMI160_RECOVER;
        nrf_bmi160_zephyr_note_recover();
        return NRF_BMI160_ERR_BUS;
    }

    /* The GY-BMI160 sits on 0x68 (SA0 low) or 0x69 (SA0 high): try both and say what each one did. */
    for (size_t i = 0U; i < sizeof(addresses); ++i) {
        const nrf_bmi160_result_t result = nrf_bmi160_init_observed(
            &attempt,
            &bus,
            addresses[i],
            nrf_bmi160_zephyr_delay_ms,
            NULL,
            bmi160_observe_state,
            NULL);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        bmi160_report(addresses[i], result, &attempt);
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
