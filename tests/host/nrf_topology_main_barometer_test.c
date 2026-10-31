#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main_barometer_fake.h"
#include "nrf_topology_main_barometer.h"

const char *test_esp_err_to_name(int error)
{
    return error == ESP_ERR_INVALID_STATE ? "ESP_ERR_INVALID_STATE" : "other";
}

static unsigned s_warnings;
static char s_last_warning[256];

void test_log_record(char level, const char *tag, const char *format, ...)
{
    (void)tag;
    if (level != 'W') return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(s_last_warning, sizeof(s_last_warning), format, args);
    va_end(args);
    ++s_warnings;
}

static void reset(nrf_topology_main_barometer_t *state)
{
    main_barometer_fake_reset();
    s_warnings = 0U;
    s_last_warning[0] = '\0';
    nrf_topology_main_barometer_init(state);
}

static void test_publishes_first_reading_then_only_changes(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    assert(g_main_barometer_fake.init_calls == 1U);
    g_main_barometer_fake.reading.bmp280_present = true;
    g_main_barometer_fake.reading.bmp280_valid = true;
    g_main_barometer_fake.reading.pressure_pa = 100650U;
    g_main_barometer_fake.reading.temperature_mc = 25080;

    nrf_topology_main_barometer_step(&state, 1000U);
    assert(g_main_barometer_fake.poll_calls == 1U && g_main_barometer_fake.last_poll_ms == 1000U);
    assert(g_main_barometer_fake.set_main_calls == 1U);
    assert(g_main_barometer_fake.last_main_status.bmp280_valid);
    assert(g_main_barometer_fake.last_main_status.pressure_pa == 100650U);
    assert(g_main_barometer_fake.last_main_status.temperature_mc == 25080);

    /* A poll that changed nothing must not take the status lock again. */
    nrf_topology_main_barometer_step(&state, 1200U);
    nrf_topology_main_barometer_step(&state, 1400U);
    assert(g_main_barometer_fake.poll_calls == 3U);
    assert(g_main_barometer_fake.set_main_calls == 1U);

    g_main_barometer_fake.reading.pressure_pa = 100648U;
    nrf_topology_main_barometer_step(&state, 1600U);
    assert(g_main_barometer_fake.set_main_calls == 2U);
    assert(g_main_barometer_fake.last_main_status.pressure_pa == 100648U);
}

static void test_height_reference_states_reach_device_status(void)
{
    const struct {
        main_barometer_reference_state_t from;
        device_main_height_reference_state_t to;
    } cases[] = {
        {MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED, DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED},
        {MAIN_BAROMETER_REFERENCE_VALID, DEVICE_MAIN_HEIGHT_REFERENCE_VALID},
        {MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE, DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE},
        {MAIN_BAROMETER_REFERENCE_SENSOR_INVALID, DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        nrf_topology_main_barometer_t state;
        reset(&state);
        g_main_barometer_fake.reading.height_reference_state = cases[i].from;
        g_main_barometer_fake.reading.height_reference_generation = (uint16_t)(7U + i);
        g_main_barometer_fake.reading.relative_z_mm = -1234;
        nrf_topology_main_barometer_step(&state, 0U);
        assert(g_main_barometer_fake.last_main_status.height_reference.state == cases[i].to);
        assert(g_main_barometer_fake.last_main_status.height_reference.generation == 7U + i);
        assert(g_main_barometer_fake.last_main_status.height_reference.relative_z_mm == -1234);
    }
}

static void test_rejected_publish_is_retried(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    g_main_barometer_fake.set_main_result = ESP_ERR_INVALID_ARG;
    nrf_topology_main_barometer_step(&state, 0U);
    nrf_topology_main_barometer_step(&state, 200U);
    assert(g_main_barometer_fake.set_main_calls == 2U);
    g_main_barometer_fake.set_main_result = ESP_OK;
    nrf_topology_main_barometer_step(&state, 400U);
    assert(g_main_barometer_fake.set_main_calls == 3U);
    nrf_topology_main_barometer_step(&state, 600U);
    assert(g_main_barometer_fake.set_main_calls == 3U);
}

static void test_poll_failures_are_logged_with_the_device_and_rate_limited(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    g_main_barometer_fake.reading.bmp280_address = 0x76U;
    g_main_barometer_fake.reading.bmp280_valid = false;
    g_main_barometer_fake.poll_result = ESP_ERR_INVALID_STATE;
    nrf_topology_main_barometer_step(&state, 10000U);
    assert(s_warnings == 1U);
    assert(strstr(s_last_warning, "I2C_BAROMETER_WARN") != NULL);
    assert(strstr(s_last_warning, "addr=0x76") != NULL);
    assert(strstr(s_last_warning, "ESP_ERR_INVALID_STATE") != NULL);

    for (uint32_t now_ms = 10200U; now_ms < 11000U; now_ms += 200U) {
        nrf_topology_main_barometer_step(&state, now_ms);
    }
    assert(s_warnings == 1U); /* rate limited */

    g_main_barometer_fake.poll_result = SENSOR_BUS_ERR_MUTEX_BUSY;
    nrf_topology_main_barometer_step(&state, 60000U);
    assert(s_warnings == 2U);
    assert(strstr(s_last_warning, "err=bus_busy") != NULL);

    g_main_barometer_fake.poll_result = ESP_OK;
    nrf_topology_main_barometer_step(&state, 120000U);
    assert(s_warnings == 2U);
}

static void test_pending_conversion_is_not_logged_as_a_bus_error(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    g_main_barometer_fake.reading.bmp280_address = 0x76U;
    /* ESP_ERR_NOT_FINISHED is the driver's normal "conversion pending / sensor re-armed" result:
     * the bench log showed it as I2C_BAROMETER_WARN at 22846 ms next to real bus errors. */
    g_main_barometer_fake.poll_result = ESP_ERR_NOT_FINISHED;
    for (uint32_t now_ms = 500000U; now_ms < 500000U + 10U * 200U; now_ms += 200U) {
        nrf_topology_main_barometer_step(&state, now_ms);
    }
    assert(s_warnings == 0U);

    /* ...and it must not use up the rate-limit window of a real error that follows. */
    g_main_barometer_fake.poll_result = ESP_ERR_INVALID_STATE;
    nrf_topology_main_barometer_step(&state, 500000U + 10U * 200U);
    assert(s_warnings == 1U);
    assert(strstr(s_last_warning, "I2C_BAROMETER_WARN") != NULL);
    assert(strstr(s_last_warning, "ESP_ERR_INVALID_STATE") != NULL);
}

static void test_undetected_barometer_logs_both_candidate_addresses(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    g_main_barometer_fake.reading.bmp280_address = 0U;
    g_main_barometer_fake.poll_result = ESP_ERR_NOT_FOUND;
    nrf_topology_main_barometer_step(&state, 10000U);
    assert(s_warnings == 1U);
    assert(strstr(s_last_warning, "addr=0x77|0x76") != NULL);
    assert(strstr(s_last_warning, "addr=0x00") == NULL);
    assert(strstr(s_last_warning, "BMP280") != NULL);
}

static void test_wait_follows_the_next_poll_and_close_is_forwarded(void)
{
    nrf_topology_main_barometer_t state;
    reset(&state);
    /* Nothing requested: the task keeps its own schedule. */
    assert(nrf_topology_main_barometer_wait_ms(&state, 0U, 5000U) == 5000U);
    assert(nrf_topology_main_barometer_wait_ms(NULL, 0U, 5000U) == 5000U);

    g_main_barometer_fake.polling_requested = true;
    g_main_barometer_fake.next_poll_delay_ms = 200U;
    nrf_topology_main_barometer_step(&state, 1000U);
    assert(nrf_topology_main_barometer_wait_ms(&state, 1000U, 5000U) == 200U);
    assert(nrf_topology_main_barometer_wait_ms(&state, 1150U, 5000U) == 50U);
    assert(nrf_topology_main_barometer_wait_ms(&state, 1300U, 5000U) == 1U);
    assert(nrf_topology_main_barometer_wait_ms(&state, 1000U, 100U) == 100U);

    nrf_topology_main_barometer_close(&state);
    assert(g_main_barometer_fake.close_calls == 1U);
    nrf_topology_main_barometer_close(NULL);
    nrf_topology_main_barometer_step(NULL, 0U);
    nrf_topology_main_barometer_init(NULL);
}

int main(void)
{
    test_publishes_first_reading_then_only_changes();
    test_height_reference_states_reach_device_status();
    test_rejected_publish_is_retried();
    test_poll_failures_are_logged_with_the_device_and_rate_limited();
    test_pending_conversion_is_not_logged_as_a_bus_error();
    test_undetected_barometer_logs_both_candidate_addresses();
    test_wait_follows_the_next_poll_and_close_is_forwarded();
    return 0;
}
