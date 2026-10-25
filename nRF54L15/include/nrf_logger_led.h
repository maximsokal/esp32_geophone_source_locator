#ifndef NRF_LOGGER_LED_H
#define NRF_LOGGER_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_node_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_LOGGER_LED_SEVERITY_NONE = 0,
    NRF_LOGGER_LED_SEVERITY_WARNING,
    NRF_LOGGER_LED_SEVERITY_ERROR,
    NRF_LOGGER_LED_SEVERITY_FATAL,
} nrf_logger_led_severity_t;

typedef enum {
    NRF_LOGGER_LED_SOURCE_BMI160_FAULT = 0,
    NRF_LOGGER_LED_SOURCE_BME280_FAULT,
    NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS,
    NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL,
    NRF_LOGGER_LED_SOURCE_CS_RANGING,
    NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE,
    NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL,
    NRF_LOGGER_LED_SOURCE_ROLE_LOST,
    NRF_LOGGER_LED_SOURCE_COUNT,
    NRF_LOGGER_LED_SOURCE_NONE = NRF_LOGGER_LED_SOURCE_COUNT,
    NRF_LOGGER_LED_SOURCE_INVALID_INPUT,
} nrf_logger_led_source_t;

/* The concrete source values are contiguous and can also be used as bit indices. */
#define NRF_LOGGER_LED_WARNING_CONFIRM_MS 2000U
#define NRF_LOGGER_LED_SOURCE_BIT(source) (UINT32_C(1) << (source))

typedef struct {
    nrf_node_state_t node_state;
    bool bmi160_init_failed;
    bool bme280_init_failed;
    bool bme280_ready;
    nrf_logger_led_severity_t source_levels[NRF_LOGGER_LED_SOURCE_COUNT];
} nrf_logger_led_input_t;

typedef struct {
    nrf_node_state_t node_state;
    bool bmi160_ready;
    bool bme280_ready;
    bool bmi160_init_failed;
    bool bme280_init_failed;
    uint32_t sensor_fault_mask;
    uint32_t log_overwritten_total;
    uint32_t log_lost_internal_total;
    uint32_t twis_protocol_error_total;
    uint32_t cs_ranging_failure_total;
    uint32_t watchdog_participant_stale_total;
    uint32_t role_lost_total;
    bool init_boot_fatal;
} nrf_logger_led_state_t;

typedef struct {
    bool green;
    bool yellow;
    bool red;
    nrf_logger_led_severity_t severity_latched;
    nrf_logger_led_source_t reason_source;
} nrf_logger_led_output_t;

typedef enum {
    NRF_LOGGER_LED_OK = 0,
    NRF_LOGGER_LED_ERR_ARGUMENT = -1,
} nrf_logger_led_result_t;

typedef struct {
    nrf_logger_led_output_t output;
    uint32_t pending_since_ms[NRF_LOGGER_LED_SOURCE_COUNT];
    nrf_logger_led_severity_t pending_levels[NRF_LOGGER_LED_SOURCE_COUNT];
    bool pending_active[NRF_LOGGER_LED_SOURCE_COUNT];
    uint32_t last_update_ms;
    uint32_t invalid_input_count;
    uint32_t timestamp_anomaly_count;
    bool has_last_update;
    /* Latch last handed out by nrf_logger_led_take_latch_change(). */
    nrf_logger_led_severity_t reported_severity;
    nrf_logger_led_source_t reported_source;
} nrf_logger_led_t;

/* Call before first use and on the node's explicit reboot/reset path. */
void nrf_logger_led_reset(nrf_logger_led_t *led);
/* Convert persistent runtime state and counters into policy source levels. */
nrf_logger_led_result_t nrf_logger_led_inputs_from_state(
    const nrf_logger_led_state_t *state,
    nrf_logger_led_input_t *input);
/* The current output, latch, and diagnostic counters are stored on led. */
nrf_logger_led_result_t nrf_logger_led_update(
    nrf_logger_led_t *led,
    const nrf_logger_led_input_t *in,
    uint32_t now_ms);

/*
 * Report a latch the log ring has not seen yet. Returns true once per change of
 * (severity_latched, reason_source) and fills the two outputs; the latch only
 * rises, so this fires at most three times between resets. Lets the node tell
 * the ESP32 which source turned the logger LED yellow or red.
 */
bool nrf_logger_led_take_latch_change(
    nrf_logger_led_t *led,
    nrf_logger_led_severity_t *severity,
    nrf_logger_led_source_t *source);

#ifdef __cplusplus
}
#endif

#endif
