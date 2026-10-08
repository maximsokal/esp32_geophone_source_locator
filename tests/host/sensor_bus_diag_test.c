#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensor_bus_diag.h"

#define TEST_ERR_OK 0
#define TEST_ERR_FAIL (-1)
#define TEST_ERR_INVALID_STATE 0x103
#define TEST_ERR_INVALID_RESPONSE 0x104
#define TEST_ERR_NOT_FOUND 0x105
#define TEST_ERR_TIMEOUT 0x107

static sensor_bus_diag_address_t *find_address(
    sensor_bus_diag_stats_t *stats,
    uint8_t address)
{
    for (size_t i = 0U; i < SENSOR_BUS_DIAG_ADDRESS_LIMIT; ++i) {
        if (stats->addresses[i].used &&
            stats->addresses[i].address == address) {
            return &stats->addresses[i];
        }
    }
    return NULL;
}

static const char *test_error_name(int err, void *context)
{
    (void)context;
    switch (err) {
        case TEST_ERR_OK: return "ESP_OK";
        case TEST_ERR_FAIL: return "ESP_FAIL";
        case TEST_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case TEST_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
        case TEST_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case TEST_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_UNKNOWN";
    }
}

typedef struct {
    char lines[SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT + 1U][256];
    size_t count;
} print_capture_t;

static void capture_line(const char *line, void *context)
{
    print_capture_t *capture = context;
    assert(capture->count < SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT + 1U);
    (void)snprintf(capture->lines[capture->count],
                   sizeof(capture->lines[capture->count]), "%s", line);
    ++capture->count;
}

static void test_rate_limit_and_sources(void)
{
    static const char source_a[] = "ina3221";
    static const char source_a_copy[] = "ina3221";
    static const char source_b[] = "nrf_topology";
    sensor_bus_diag_state_t state = { 0 };
    char line[192];

    assert(sensor_bus_diag_format_limited(
        &state, 100U, source_a, 0x40U, -1, "ESP_ERR_TIMEOUT", 1, 0,
        line, sizeof(line)));
    assert(!sensor_bus_diag_format_limited(
        &state, 5099U, source_a, 0x40U, -1, "ESP_ERR_TIMEOUT", 1, 0,
        line, sizeof(line)));
    assert(!sensor_bus_diag_rate_limit_allow(&state, 5099U, source_a_copy));
    assert(sensor_bus_diag_format_limited(
        &state, 5100U, source_a, 0x40U, -1, "ESP_ERR_TIMEOUT", 1, 0,
        line, sizeof(line)));
    assert(sensor_bus_diag_format_limited(
        &state, 101U, source_b, 0x30U, 2, "ESP_ERR_TIMEOUT", 0, 1,
        line, sizeof(line)));
}

static void test_wraparound(void)
{
    static const char source[] = "wraparound";
    sensor_bus_diag_state_t state = { 0 };
    const uint32_t first_ms = UINT32_MAX - 2000U;

    assert(sensor_bus_diag_rate_limit_allow(&state, first_ms, source));
    assert(!sensor_bus_diag_rate_limit_allow(
        &state, (uint32_t)(first_ms + 4999U), source));
    assert(sensor_bus_diag_rate_limit_allow(
        &state, (uint32_t)(first_ms + 5000U), source));
}

static void test_device_names_and_timeout_formatting(void)
{
    char line[192];

    assert(strcmp(sensor_bus_diag_device_name(0x38U, "main_barometer"), "AHT20") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x40U, "ina3221"), "INA3221") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x70U, "tca9548a"), "TCA9548A") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x76U, "main_barometer"), "BMP280") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x77U, "tca9548a"), "TCA9548A") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x76U, "probe"), "TCA9548A/BMP280?") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x30U, "nrf_topology"), "XIAO_UPSTREAM") == 0);
    assert(strcmp(sensor_bus_diag_device_name(0x55U, "probe"), "unknown") == 0);

    assert(sensor_bus_diag_format(
        line, sizeof(line), "ina3221", 0x40U, -1,
        "ESP_ERR_TIMEOUT", 0, 1));
    assert(strcmp(line,
        "I2C_TIMEOUT_DIAG src=ina3221 addr=0x40 dev=INA3221 ch=direct err=ESP_ERR_TIMEOUT sda=0 scl=1") == 0);

    assert(sensor_bus_diag_format(
        line, sizeof(line), "nrf_topology", 0x30U, 3,
        "ESP_ERR_INVALID_STATE", 1, 0));
    assert(strcmp(line,
        "I2C_TIMEOUT_DIAG src=nrf_topology addr=0x30 dev=XIAO_UPSTREAM ch=3 err=ESP_ERR_INVALID_STATE sda=1 scl=0") == 0);

    assert(sensor_bus_diag_format(
        line, sizeof(line), NULL, 0x55U, -1,
        "ESP_ERR_TIMEOUT", -1, -1));
    assert(strcmp(line,
        "I2C_TIMEOUT_DIAG src=bus addr=0x55 dev=unknown ch=direct err=ESP_ERR_TIMEOUT sda=? scl=?") == 0);
}

static void test_address_statistics_count_classes(void)
{
    static const char source[] = "ina3221";
    sensor_bus_diag_stats_t stats = { 0 };
    sensor_bus_diag_address_t *entry;

    assert(sensor_bus_diag_record_result(
        &stats, 100U, 0x40U, -1, TEST_ERR_OK,
        SENSOR_BUS_DIAG_RESULT_OK, -1, -1, source));
    assert(sensor_bus_diag_record_result(
        &stats, 200U, 0x40U, 2, TEST_ERR_NOT_FOUND,
        SENSOR_BUS_DIAG_RESULT_NACK, 0, 1, source));
    assert(sensor_bus_diag_record_result(
        &stats, 300U, 0x40U, 2, TEST_ERR_INVALID_STATE,
        SENSOR_BUS_DIAG_RESULT_NACK_INVALID_STATE, 0, 0, source));
    assert(sensor_bus_diag_record_result(
        &stats, 400U, 0x40U, 3, TEST_ERR_TIMEOUT,
        SENSOR_BUS_DIAG_RESULT_TIMEOUT, 1, 0, source));
    assert(sensor_bus_diag_record_result(
        &stats, 500U, 0x40U, -1, TEST_ERR_INVALID_RESPONSE,
        SENSOR_BUS_DIAG_RESULT_OTHER, 1, 1, source));

    entry = find_address(&stats, 0x40U);
    assert(entry != NULL);
    assert(entry->ok_count == 1U);
    assert(entry->nack_count == 2U);
    assert(entry->nack_invalid_state_count == 1U);
    assert(entry->timeout_count == 1U);
    assert(entry->other_count == 1U);
    assert(entry->last_err == TEST_ERR_INVALID_RESPONSE);
    assert(entry->last_channel == -1);
    assert(entry->last_sda == 1);
    assert(entry->last_scl == 1);
    assert(entry->last_ok_ms == 100U);
    assert(entry->last_failure_ms == 500U);
    assert(strcmp(entry->last_failure_source, source) == 0);
}

static void test_summary_window_format_and_healthy_suppression(void)
{
    static const char source[] = "ina3221";
    sensor_bus_diag_stats_t stats = { 0 };
    sensor_bus_diag_summary_t summary;
    print_capture_t capture = { 0 };

    assert(!sensor_bus_diag_summary_capture(&stats, 0U, &summary));
    assert(!summary.should_emit);
    assert(!sensor_bus_diag_summary_capture(&stats, 10000U, &summary));
    assert(!summary.should_emit);
    assert(capture.count == 0U);

    assert(sensor_bus_diag_record_result(
        &stats, 11000U, 0x40U, -1, TEST_ERR_NOT_FOUND,
        SENSOR_BUS_DIAG_RESULT_NACK, 0, 1, source));
    assert(!sensor_bus_diag_summary_capture(&stats, 19999U, &summary));
    assert(sensor_bus_diag_summary_capture(&stats, 20000U, &summary));
    assert(summary.should_emit);
    assert(summary.address_count == 1U);
    assert(sensor_bus_diag_emit_summary(
        &summary, 4U, 2U, 1U, 1U, 1, 0,
        test_error_name, capture_line, &capture) == 2U);
    assert(capture.count == 2U);
    assert(strcmp(capture.lines[0],
        "I2C_BUS_SUMMARY recover_attempts=4 recoveries=2 recover_failures=1 recover_consecutive=1 table_overflow=0 sda=1 scl=0") == 0);
    assert(strcmp(capture.lines[1],
        "I2C_BUS_SUMMARY addr=0x40 dev=INA3221 ok=0 nack=1 timeout=0 other=0 last_err=ESP_ERR_NOT_FOUND last_ch=direct sda=0 scl=1 age_ok_ms=4294967295 age_fail_ms=9000") == 0);

    memset(&capture, 0, sizeof(capture));
    assert(!sensor_bus_diag_summary_capture(&stats, 30000U, &summary));
    assert(!summary.should_emit);
    assert(sensor_bus_diag_emit_summary(
        &summary, 0U, 0U, 0U, 0U, 1, 1,
        test_error_name, capture_line, &capture) == 0U);
    assert(capture.count == 0U);
}

static void test_first_summary_includes_existing_recent_failure(void)
{
    static const char source[] = "nrf_topology";
    sensor_bus_diag_stats_t stats = { 0 };
    sensor_bus_diag_summary_t summary;

    assert(sensor_bus_diag_record_result(
        &stats, 5000U, 0x30U, 3, TEST_ERR_TIMEOUT,
        SENSOR_BUS_DIAG_RESULT_TIMEOUT, 0, 1, source));
    assert(sensor_bus_diag_summary_capture(&stats, 6000U, &summary));
    assert(summary.address_count == 1U);
    assert(summary.addresses[0].address == 0x30U);
}

static void test_address_table_overflow_and_summary_cap(void)
{
    static const char source[] = "probe";
    sensor_bus_diag_stats_t stats = { 0 };
    sensor_bus_diag_summary_t summary;
    print_capture_t capture = { 0 };

    assert(!sensor_bus_diag_summary_capture(&stats, 0U, &summary));
    for (uint8_t address = 0U;
         address < SENSOR_BUS_DIAG_ADDRESS_LIMIT;
         ++address) {
        assert(sensor_bus_diag_record_result(
            &stats, 100U, address, -1, TEST_ERR_TIMEOUT,
            SENSOR_BUS_DIAG_RESULT_TIMEOUT, 0, 0, source));
    }
    assert(!sensor_bus_diag_record_result(
        &stats, 200U, (uint8_t)SENSOR_BUS_DIAG_ADDRESS_LIMIT, -1,
        TEST_ERR_TIMEOUT, SENSOR_BUS_DIAG_RESULT_TIMEOUT, 0, 0, source));
    assert(stats.overflow_count == 1U);
    assert(stats.last_overflow_ms == 200U);

    assert(sensor_bus_diag_summary_capture(&stats, 10000U, &summary));
    assert(summary.address_count == SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT);
    assert(sensor_bus_diag_emit_summary(
        &summary, 0U, 0U, 0U, 0U, 0, 0,
        test_error_name, capture_line, &capture) ==
        SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT + 1U);
    assert(capture.count == SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT + 1U);
    assert(strstr(capture.lines[0], "table_overflow=1") != NULL);

    memset(&capture, 0, sizeof(capture));
    assert(!sensor_bus_diag_summary_capture(&stats, 20000U, &summary));
    for (uint8_t address = 0U;
         address < SENSOR_BUS_DIAG_ADDRESS_LIMIT;
         ++address) {
        assert(sensor_bus_diag_record_result(
            &stats, 20100U, address, -1, TEST_ERR_TIMEOUT,
            SENSOR_BUS_DIAG_RESULT_TIMEOUT, 0, 0, source));
    }
    assert(sensor_bus_diag_summary_capture(&stats, 30000U, &summary));
    assert(summary.address_count == SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT);
    assert(summary.addresses[0].address == SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT);
}

static void test_rate_limit_table_overflow(void)
{
    sensor_bus_diag_state_t state = { 0 };
    char sources[SENSOR_BUS_DIAG_SOURCE_LIMIT + 1U][24];

    for (size_t i = 0U; i < SENSOR_BUS_DIAG_SOURCE_LIMIT; ++i) {
        (void)snprintf(sources[i], sizeof(sources[i]), "source-%u", (unsigned)i);
        assert(sensor_bus_diag_rate_limit_allow(&state, 10U, sources[i]));
    }
    (void)snprintf(sources[SENSOR_BUS_DIAG_SOURCE_LIMIT],
                   sizeof(sources[SENSOR_BUS_DIAG_SOURCE_LIMIT]), "source-overflow");
    assert(!sensor_bus_diag_rate_limit_allow(
        &state, 10U, sources[SENSOR_BUS_DIAG_SOURCE_LIMIT]));
    assert(sensor_bus_diag_rate_limit_allow(&state, 5010U, sources[0]));
}

int main(void)
{
    test_rate_limit_and_sources();
    test_wraparound();
    test_device_names_and_timeout_formatting();
    test_address_statistics_count_classes();
    test_summary_window_format_and_healthy_suppression();
    test_first_summary_includes_existing_recent_failure();
    test_address_table_overflow_and_summary_cap();
    test_rate_limit_table_overflow();
    return 0;
}
