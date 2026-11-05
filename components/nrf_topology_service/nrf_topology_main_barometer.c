#include "nrf_topology_main_barometer.h"

#include <string.h>

#include "esp_log.h"
#include "sensor_bus.h"
#include "sensor_bus_diag.h"

static const char *TAG = "MAIN_BARO";

/*
 * How long a sample stays "valid" for the status consumers after the poll that produced it. The driver
 * clears bmp280_valid on every poll that finds no new conversion yet (about three polls in four with a
 * continuously converting sensor), but the pressure and temperature it holds are still the current ones.
 * The placement-stability check needs MAIN valid on every poll and wiped its history each time, so the
 * stability screen never completed (2026-10-31 16:24).
 */
#define MAIN_BARO_PUBLISH_HOLD_MS 2000U
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
        a->pressure_pa == b->pressure_pa &&
        a->temperature_mc == b->temperature_mc &&
        a->barometer_sample_ms == b->barometer_sample_ms &&
        a->barometer_filtered_altitude_mm == b->barometer_filtered_altitude_mm &&
        a->height_reference.state == b->height_reference.state &&
        a->height_reference.generation == b->height_reference.generation &&
        a->height_reference.relative_z_mm == b->height_reference.relative_z_mm;
}

static void publish(nrf_topology_main_barometer_t *state, uint32_t now_ms, esp_err_t poll_err)
{
    main_barometer_reading_t reading = {0};
    main_barometer_get_reading(&state->barometer, &reading);
    /* Only "no new conversion yet" keeps the previous sample alive; a real error never does. */
    const bool hold_allowed = poll_err == ESP_OK || poll_err == ESP_ERR_NOT_FINISHED;
    const bool sample_current = reading.bmp280_present && reading.last_sample_ms != 0U &&
        now_ms >= reading.last_sample_ms &&
        now_ms - reading.last_sample_ms <= MAIN_BARO_PUBLISH_HOLD_MS;
    const device_main_status_t status = {
        .bmp280_present = reading.bmp280_present,
        .bmp280_valid = reading.bmp280_valid || (hold_allowed && sample_current),
        .pressure_pa = reading.pressure_pa,
        .temperature_mc = reading.temperature_mc,
        .barometer_sample_ms = reading.last_sample_ms,
        .barometer_filtered_altitude_mm = reading.filtered_altitude_mm,
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
    if (err == ESP_ERR_NOT_FINISHED) {
        /* Expected FSM state (conversion pending, or a reset sensor just re-armed), not a bus error:
         * keep it out of I2C_BAROMETER_WARN and out of the rate-limit window real errors need. */
        return;
    }
    if (err == ESP_OK || !sensor_bus_diag_rate_limit_allow(
            &s_diag_state, now_ms, "main_barometer")) {
        return;
    }

    main_barometer_reading_t reading = {0};
    main_barometer_get_reading(&state->barometer, &reading);
    const uint8_t address = reading.bmp280_address;
    if (reading.failure_stage == MAIN_BAROMETER_FAILURE_CONFIG_VERIFY ||
        reading.failure_stage == MAIN_BAROMETER_FAILURE_COMPENSATION ||
        reading.failure_stage == MAIN_BAROMETER_FAILURE_FILTER) {
        ESP_LOGW(TAG, "MAIN_BARO_WARN addr=0x%02X stage=%s err=%s",
                 (unsigned)address,
                 reading.failure_stage == MAIN_BAROMETER_FAILURE_CONFIG_VERIFY ? "config_verify" :
                 reading.failure_stage == MAIN_BAROMETER_FAILURE_COMPENSATION ? "compensation" : "filter_update",
                 error_name(err));
        return;
    }
    if (address == 0U) {
        /* Not detected yet: both candidates were probed, so do not report a bogus 0x00. */
        ESP_LOGW(TAG,
                 "I2C_BAROMETER_WARN src=main_barometer addr=0x77|0x76 dev=BMP280 (not detected) err=%s",
                 error_name(err));
        return;
    }
    ESP_LOGW(TAG,
             "I2C_BAROMETER_WARN src=main_barometer addr=0x%02X dev=%s err=%s",
             (unsigned)address,
             sensor_bus_diag_device_name(address, "main_barometer"),
             error_name(err));
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
    publish(state, (uint32_t)now_ms, err);
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
