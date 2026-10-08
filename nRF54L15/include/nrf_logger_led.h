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
    bool bme280_ready;
    nrf_logger_led_severity_t source_levels[NRF_LOGGER_LED_SOURCE_COUNT];
} nrf_logger_led_input_t;

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
} nrf_logger_led_t;

/* Call before first use and on the node's explicit reboot/reset path. */
void nrf_logger_led_reset(nrf_logger_led_t *led);
/* The current output, latch, and diagnostic counters are stored on led. */
nrf_logger_led_result_t nrf_logger_led_update(
    nrf_logger_led_t *led,
    const nrf_logger_led_input_t *in,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
