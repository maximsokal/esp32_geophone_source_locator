#include "nrf_log_event_text.h"

#include <inttypes.h>
#include <stdio.h>

#include "nrf_log_events.h"
#include "nrf_log_protocol.h"
#include "nrf_logger_led.h"

const char *nrf_log_event_text_name(uint16_t event_id)
{
    if (event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID) {
        return "ESP_REBOOT_MARKER";
    }
    switch (event_id) {
#define X(id, name, subsystem, severity) case id: return #name;
#include "nrf_log_events.def"
#undef X
    default:
        return "UNKNOWN_EVENT";
    }
}

const char *nrf_log_event_text_severity(uint8_t severity)
{
    switch (severity) {
    case NRF_LOG_SEVERITY_DEBUG: return "DEBUG";
    case NRF_LOG_SEVERITY_INFO: return "INFO";
    case NRF_LOG_SEVERITY_WARN: return "WARN";
    case NRF_LOG_SEVERITY_ERROR: return "ERROR";
    case NRF_LOG_SEVERITY_FATAL: return "FATAL";
    default: return "?";
    }
}

const char *nrf_log_event_text_subsystem(uint8_t subsystem)
{
    switch (subsystem) {
    case NRF_LOG_SUBSYSTEM_BOOT: return "BOOT";
    case NRF_LOG_SUBSYSTEM_LOG: return "LOG";
    case NRF_LOG_SUBSYSTEM_TWIS: return "TWIS";
    case NRF_LOG_SUBSYSTEM_TOPOLOGY: return "TOPOLOGY";
    case NRF_LOG_SUBSYSTEM_SENSOR: return "SENSOR";
    case NRF_LOG_SUBSYSTEM_CALIBRATION: return "CALIBRATION";
    case NRF_LOG_SUBSYSTEM_CS: return "CS";
    case NRF_LOG_SUBSYSTEM_WATCHDOG: return "WATCHDOG";
    case NRF_LOG_SUBSYSTEM_POWER: return "POWER";
    case NRF_LOG_SUBSYSTEM_FAULT: return "FAULT";
    default: return "?";
    }
}

const char *nrf_log_event_text_logger_source(uint32_t source)
{
    switch (source) {
    case NRF_LOGGER_LED_SOURCE_BMI160_FAULT: return "BMI160_FAULT";
    case NRF_LOGGER_LED_SOURCE_BME280_FAULT: return "BME280_FAULT";
    case NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS: return "LOG_RING_LOSS";
    case NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL: return "TWIS_PROTOCOL";
    case NRF_LOGGER_LED_SOURCE_CS_RANGING: return "CS_RANGING";
    case NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE: return "WATCHDOG_PARTICIPANT_STALE";
    case NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL: return "INIT_BOOT_FATAL";
    case NRF_LOGGER_LED_SOURCE_ROLE_LOST: return "ROLE_LOST";
    default: return "?";
    }
}

static const char *logger_severity_name(uint32_t severity)
{
    switch (severity) {
    case NRF_LOGGER_LED_SEVERITY_NONE: return "NONE";
    case NRF_LOGGER_LED_SEVERITY_WARNING: return "WARNING";
    case NRF_LOGGER_LED_SEVERITY_ERROR: return "ERROR";
    case NRF_LOGGER_LED_SEVERITY_FATAL: return "FATAL";
    default: return "?";
    }
}

size_t nrf_log_event_text_format(
    char *buffer,
    size_t size,
    uint8_t node_index,
    const nrf_log_stored_record_t *record)
{
    if (record == NULL) {
        if (buffer != NULL && size > 0U) {
            buffer[0] = '\0';
        }
        return 0U;
    }

    int written = snprintf(
        buffer, size,
        "NRF_EVT node=%u boot=%" PRIu32 " seq=%" PRIu32 " t_ms=%" PRIu32
        " sev=%s sub=%s ev=%s(%u) a0=%" PRIu32 " a1=%" PRIu32,
        (unsigned)node_index,
        record->boot_id,
        record->seq,
        record->uptime_ms,
        nrf_log_event_text_severity(record->severity),
        nrf_log_event_text_subsystem(record->subsystem),
        nrf_log_event_text_name(record->event_id),
        (unsigned)record->event_id,
        record->arg0,
        record->arg1);
    if (written < 0) {
        if (buffer != NULL && size > 0U) {
            buffer[0] = '\0';
        }
        return 0U;
    }

    size_t length = (size_t)written;
    if (record->event_id == NRF_LOG_EVENT_LOGGER_LATCH) {
        const size_t used = length < size ? length : (size > 0U ? size - 1U : 0U);
        written = snprintf(
            buffer != NULL && size > 0U ? buffer + used : NULL,
            size > used ? size - used : 0U,
            " latch=%s/%s",
            nrf_log_event_text_logger_source(record->arg0),
            logger_severity_name(record->arg1));
        if (written > 0) {
            length += (size_t)written;
        }
    }
    return length;
}
