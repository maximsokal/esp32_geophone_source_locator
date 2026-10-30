#include "nrf_topology_main_barometer.h"

#include <string.h>

#include "esp_log.h"
#include "sensor_bus.h"
#include "sensor_bus_diag.h"

static const char *TAG = "MAIN_BARO";
static sensor_bus_diag_state_t s_diag_state;

/* The shared-bus owner codes are not in esp_err_to_name(), which prints "ERROR". */
static const char *error_name(esp_err_t err)
{
    if (err == SENSOR_BUS_ERR_MUTEX_BUSY) {
        return "bus_busy";
    }
    if (err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        return "bus_hold_budget";
    }
    return esp_err_to_name(err);
}

static device_main_height_reference_state_t reference_state(
    main_barometer_reference_state_t state)
{
    switch (state) {
        case MAIN_BAROMETER_REFERENCE_VALID:
            return DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
        case MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE:
            return DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE;
        case MAIN_BAROMETER_REFERENCE_SENSOR_INVALID:
            return DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID;
        case MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED:
        default:
            return DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED;
    }
}

static bool status_equal(const device_main_status_t *a, const device_main_status_t *b)
{
    return a->bmp280_present == b->bmp280_present &&
        a->bmp280_valid == b->bmp280_valid &&
        a->aht20_present == b->aht20_present &&
        a->aht20_valid == b->aht20_valid &&
        a->pressure_pa == b->pressure_pa &&
        a->temperature_mc == b->temperature_mc &&
        a->humidity_mpct == b->humidity_mpct &&
        a->height_reference.state == b->height_reference.state &&
        a->height_reference.generation == b->height_reference.generation &&
        a->height_reference.relative_z_mm == b->height_reference.relative_z_mm;
}

static void publish(nrf_topology_main_barometer_t *state)
{
    main_barometer_reading_t reading = {0};
    main_barometer_get_reading(&state->barometer, &reading);
    const device_main_status_t status = {
        .bmp280_present = reading.bmp280_present,
        .bmp280_valid = reading.bmp280_valid,
        .aht20_present = reading.aht20_present,
        .aht20_valid = reading.aht20_valid,
        .pressure_pa = reading.pressure_pa,
        .temperature_mc = reading.temperature_mc,
        .humidity_mpct = reading.humidity_mpct,
        .height_reference = {
            .state = reference_state(reading.height_reference_state),
            .generation = reading.height_reference_generation,
            .relative_z_mm = reading.relative_z_mm,
        },
    };
    if (state->published && status_equal(&status, &state->last_status)) {
        return;
    }
    if (device_status_set_main(&status) == ESP_OK) {
        state->last_status = status;
        state->published = true;
    }
}

static void log_error(nrf_topology_main_barometer_t *state, esp_err_t err, uint32_t now_ms)
{
    if (err == ESP_OK || !sensor_bus_diag_rate_limit_allow(
            &s_diag_state, now_ms, "main_barometer")) {
        return;
    }

    main_barometer_reading_t reading = {0};
    main_barometer_get_reading(&state->barometer, &reading);
    const bool bmp280_invalid =
        reading.bmp280_address != 0U && !reading.bmp280_valid;
    const bool aht20_invalid =
        reading.aht20_present && !reading.aht20_valid;
    if (bmp280_invalid && aht20_invalid) {
        ESP_LOGW(TAG,
                 "I2C_BAROMETER_WARN src=main_barometer bmp_addr=0x%02X bmp_dev=%s aht20_addr=0x38 aht20_dev=%s err=%s",
                 (unsigned)reading.bmp280_address,
                 sensor_bus_diag_device_name(
                     reading.bmp280_address, "main_barometer"),
                 sensor_bus_diag_device_name(0x38U, "main_barometer"),
                 error_name(err));
    } else if (bmp280_invalid) {
        ESP_LOGW(TAG,
                 "I2C_BAROMETER_WARN src=main_barometer addr=0x%02X dev=%s err=%s",
                 (unsigned)reading.bmp280_address,
                 sensor_bus_diag_device_name(
                     reading.bmp280_address, "main_barometer"),
                 error_name(err));
    } else if (aht20_invalid) {
        ESP_LOGW(TAG,
                 "I2C_BAROMETER_WARN src=main_barometer addr=0x38 dev=%s err=%s",
                 sensor_bus_diag_device_name(0x38U, "main_barometer"),
                 error_name(err));
    } else {
        const uint8_t address = reading.bmp280_address != 0U
            ? reading.bmp280_address
            : reading.aht20_present ? 0x38U : 0U;
        ESP_LOGW(TAG,
                 "I2C_BAROMETER_WARN src=main_barometer addr=0x%02X dev=%s err=%s",
                 (unsigned)address,
                 sensor_bus_diag_device_name(address, "main_barometer"),
                 error_name(err));
    }
}

void nrf_topology_main_barometer_init(nrf_topology_main_barometer_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    main_barometer_init(&state->barometer);
}

void nrf_topology_main_barometer_step(
    nrf_topology_main_barometer_t *state,
    uint64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    const esp_err_t err = main_barometer_poll(&state->barometer, now_ms);
    log_error(state, err, (uint32_t)now_ms);
    publish(state);
}

uint32_t nrf_topology_main_barometer_wait_ms(
    const nrf_topology_main_barometer_t *state,
    uint64_t now_ms,
    uint32_t max_ms)
{
    return state == NULL
        ? max_ms
        : main_barometer_ms_until_poll(&state->barometer, now_ms, max_ms);
}

void nrf_topology_main_barometer_close(nrf_topology_main_barometer_t *state)
{
    if (state == NULL) {
        return;
    }
    main_barometer_close(&state->barometer);
}
