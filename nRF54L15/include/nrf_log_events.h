#ifndef NRF_LOG_EVENTS_H
#define NRF_LOG_EVENTS_H

#include "nrf_log_protocol.h"

typedef enum {
#define X(id, name, subsystem, severity) NRF_LOG_EVENT_##name = id,
#include "nrf_log_events.def"
#undef X
} nrf_log_event_id_t;

/*
 * Event argument table (kept beside the .def list; arguments are uint32_t):
 * Event                     arg0                                   arg1
 * BOOT                      reset-cause bits (0 if unknown)         boot-id fallback flag (0/1)
 * LOG_GENERATION_ROLLOVER   previous boot id                        new boot id
 * TWIS_INVALID_FRAME        raw message type (0 if absent)          type-specific decoder error code
 * TWIS_UNKNOWN_TYPE         raw message type                        frame length in bytes
 * TWIS_BUSY                 raw message type                        capacity of the full work queue
 * ROLE_ASSIGNED             assigned node id                        assigned role enum
 * ROLE_LOST                 previous node id                        previous role enum
 * IMU_FAULT_SET/CLEARED     IMU fault-mask bit                      snapshot sequence
 * ENV_FAULT_SET/CLEARED     ENV fault-mask bit                      snapshot sequence
 * HEIGHT_REF_REJECTED       request id                               rejected reference-state enum
 * IMU_CAL_REJECTED          request id                               rejected calibration-state enum
 * CS_EXECUTE_FAILED         attempt id                               transport/outcome error code
 * CS_TIMEOUT                attempt id                               timeout in milliseconds
 * WATCHDOG_PARTICIPANT_STALE participant enum                        allowed silence in milliseconds
 * LOG_TEST_EVENT            monotonically increasing test index      uptime in milliseconds
 * FAULT_RECORD_WORD         fault-record word index                  little-endian word value
 * BOOT_FAILED               stable boot-failure reason               raw errno
 * CS_EVENT_QUEUE_OVERFLOW   saturating queue-overflow total           event queue capacity
 * WATCHDOG_DEGRADED         watchdog arm-failure stage                raw errno
 * WATCHDOG_FEED_FAILED      watchdog channel id                       raw feed error
 * SENSOR_INIT_FAILED        sensor bit (1 BMI160, 2 BME280)           signed driver init result as uint32
 * LOGGER_LATCH              nrf_logger_led_source_t of the latch    nrf_logger_led_severity_t (1 WARNING, 2 ERROR, 3 FATAL)
 *                           (the record's own boot id and uptime say when and in which boot the LED latched;
 *                           the record severity is the event class WARN, arg1 carries the latched severity)
 */

#endif
