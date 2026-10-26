#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_log_events.h"
#include "nrf_log_event_text.h"

static nrf_log_stored_record_t record(uint16_t event_id, uint8_t severity, uint8_t subsystem,
    uint32_t arg0, uint32_t arg1)
{
    nrf_log_stored_record_t value;
    memset(&value, 0, sizeof(value));
    value.boot_id = 77U;
    value.seq = 12U;
    value.uptime_ms = 5300U;
    value.event_id = event_id;
    value.severity = severity;
    value.subsystem = subsystem;
    value.arg0 = arg0;
    value.arg1 = arg1;
    return value;
}

static void test_every_event_in_the_def_list_has_its_name(void)
{
#define X(id, name, subsystem, severity) \
    assert(strcmp(nrf_log_event_text_name(id), #name) == 0);
#include "nrf_log_events.def"
#undef X
    assert(strcmp(nrf_log_event_text_name(9999U), "UNKNOWN_EVENT") == 0);
    assert(strcmp(nrf_log_event_text_name(NRF_LOG_COLLECTOR_REBOOT_EVENT_ID),
                  "ESP_REBOOT_MARKER") == 0);
}

static void test_logger_latch_line_names_the_source_and_severity(void)
{
    char line[160];
    const nrf_log_stored_record_t latch = record(
        NRF_LOG_EVENT_LOGGER_LATCH, NRF_LOG_SEVERITY_WARN, NRF_LOG_SUBSYSTEM_LOG, 4U, 2U);
    const size_t length = nrf_log_event_text_format(line, sizeof(line), 4U, &latch);
    assert(strcmp(line,
        "NRF_EVT node=4 boot=77 seq=12 t_ms=5300 sev=WARN sub=LOG "
        "ev=LOGGER_LATCH(24) a0=4 a1=2 latch=CS_RANGING/ERROR") == 0);
    assert(length == strlen(line));
}

static void test_plain_and_reboot_marker_lines(void)
{
    char line[160];
    const nrf_log_stored_record_t sensor = record(
        NRF_LOG_EVENT_SENSOR_INIT_FAILED, NRF_LOG_SEVERITY_ERROR, NRF_LOG_SUBSYSTEM_SENSOR, 2U,
        (uint32_t)-5);
    (void)nrf_log_event_text_format(line, sizeof(line), 1U, &sensor);
    assert(strcmp(line,
        "NRF_EVT node=1 boot=77 seq=12 t_ms=5300 sev=ERROR sub=SENSOR "
        "ev=SENSOR_INIT_FAILED(23) a0=2 a1=4294967291") == 0);

    nrf_log_stored_record_t marker = record(
        NRF_LOG_COLLECTOR_REBOOT_EVENT_ID, NRF_LOG_SEVERITY_WARN, NRF_LOG_SUBSYSTEM_BOOT, 5U, 6U);
    marker.seq = UINT32_MAX;
    (void)nrf_log_event_text_format(line, sizeof(line), 0U, &marker);
    assert(strstr(line, "ev=ESP_REBOOT_MARKER(65535) a0=5 a1=6") != NULL);
    assert(strstr(line, "latch=") == NULL);
}

static void test_unknown_latch_source_is_marked_not_crashed(void)
{
    char line[160];
    const nrf_log_stored_record_t latch = record(
        NRF_LOG_EVENT_LOGGER_LATCH, NRF_LOG_SEVERITY_WARN, NRF_LOG_SUBSYSTEM_LOG, 99U, 9U);
    (void)nrf_log_event_text_format(line, sizeof(line), 0U, &latch);
    assert(strstr(line, "latch=?/?") != NULL);
}

static void test_truncation_is_safe_and_reports_full_length(void)
{
    char full[160];
    char small[24];
    const nrf_log_stored_record_t latch = record(
        NRF_LOG_EVENT_LOGGER_LATCH, NRF_LOG_SEVERITY_WARN, NRF_LOG_SUBSYSTEM_LOG, 4U, 2U);
    const size_t full_length = nrf_log_event_text_format(full, sizeof(full), 4U, &latch);

    memset(small, 'x', sizeof(small));
    assert(nrf_log_event_text_format(small, sizeof(small), 4U, &latch) == full_length);
    assert(small[sizeof(small) - 1U] == '\0');
    assert(strncmp(small, full, sizeof(small) - 1U) == 0);

    assert(nrf_log_event_text_format(NULL, 0U, 4U, &latch) == full_length);
    assert(nrf_log_event_text_format(small, sizeof(small), 4U, NULL) == 0U);
    assert(small[0] == '\0');
}

int main(void)
{
    test_every_event_in_the_def_list_has_its_name();
    test_logger_latch_line_names_the_source_and_severity();
    test_plain_and_reboot_marker_lines();
    test_unknown_latch_source_is_marked_not_crashed();
    test_truncation_is_safe_and_reports_full_length();
    return 0;
}
