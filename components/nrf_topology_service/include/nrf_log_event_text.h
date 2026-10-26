#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nrf_log_collector.h"

/* Names come from nrf_log_events.def, so they cannot drift from the nRF side. */
const char *nrf_log_event_text_name(uint16_t event_id);
const char *nrf_log_event_text_severity(uint8_t severity);
const char *nrf_log_event_text_subsystem(uint8_t subsystem);
const char *nrf_log_event_text_logger_source(uint32_t source);

/*
 * One compact, grep-able line for the serial log (so a monitor that saves the
 * ESP32 console also saves the nRF logs the ESP32 fetched over I2C):
 *   NRF_EVT node=4 boot=77 seq=12 t_ms=5300 sev=WARN sub=LOG ev=LOGGER_LATCH(24) a0=2 a1=2 latch=CS_RANGING/ERROR
 * Always NUL-terminates when size > 0 and returns the untruncated length.
 */
size_t nrf_log_event_text_format(
    char *buffer,
    size_t size,
    uint8_t node_index,
    const nrf_log_stored_record_t *record);
