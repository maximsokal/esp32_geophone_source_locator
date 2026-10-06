#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSOR_BUS_DIAG_SOURCE_LIMIT 8U
#define SENSOR_BUS_DIAG_ADDRESS_LIMIT 32U
/* Each emitted block has one header and at most eight address rows. */
#define SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT 8U
#define SENSOR_BUS_DIAG_INTERVAL_MS 5000U
#define SENSOR_BUS_DIAG_SUMMARY_INTERVAL_MS 10000U

typedef struct {
    const char *source;
    uint32_t last_emit_ms;
    bool used;
} sensor_bus_diag_slot_t;

typedef struct {
    sensor_bus_diag_slot_t slots[SENSOR_BUS_DIAG_SOURCE_LIMIT];
} sensor_bus_diag_state_t;

typedef enum {
    SENSOR_BUS_DIAG_RESULT_OK = 0,
    SENSOR_BUS_DIAG_RESULT_NACK,
    SENSOR_BUS_DIAG_RESULT_NACK_INVALID_STATE,
    SENSOR_BUS_DIAG_RESULT_TIMEOUT,
    SENSOR_BUS_DIAG_RESULT_OTHER,
} sensor_bus_diag_result_t;

typedef struct {
    uint8_t address;
    uint8_t used;
    uint32_t ok_count;
    uint32_t nack_count;
    uint32_t nack_invalid_state_count;
    uint32_t timeout_count;
    uint32_t other_count;
    /* Error, channel, levels, and source describe the latest failure. */
    int last_err;
    int8_t last_channel;
    int8_t last_sda;
    int8_t last_scl;
    uint32_t last_ok_ms;
    uint32_t last_failure_ms;
    char last_failure_source[24];
} sensor_bus_diag_address_t;

typedef struct {
    sensor_bus_diag_address_t addresses[SENSOR_BUS_DIAG_ADDRESS_LIMIT];
    uint32_t overflow_count;
    uint32_t last_overflow_ms;
    uint32_t last_summary_ms;
    size_t summary_cursor;
    bool summary_started;
} sensor_bus_diag_stats_t;

typedef struct {
    sensor_bus_diag_address_t addresses[SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT];
    size_t address_count;
    uint32_t now_ms;
    uint32_t overflow_count;
    bool should_emit;
} sensor_bus_diag_summary_t;

typedef const char *(*sensor_bus_diag_error_name_fn)(int err, void *context);
typedef void (*sensor_bus_diag_print_fn)(const char *line, void *context);

/* Pure bounded state operations; serialize shared instances in the caller. */
/* Rate-limit source strings must remain valid for the lifetime of the state. */
bool sensor_bus_diag_rate_limit_allow(
    sensor_bus_diag_state_t *state,
    uint32_t now_ms,
    const char *source);

bool sensor_bus_diag_format(
    char *buffer,
    size_t buffer_size,
    const char *source,
    uint8_t address,
    int channel,
    const char *error_name,
    int sda,
    int scl);

bool sensor_bus_diag_format_limited(
    sensor_bus_diag_state_t *state,
    uint32_t now_ms,
    const char *source,
    uint8_t address,
    int channel,
    const char *error_name,
    int sda,
    int scl,
    char *buffer,
    size_t buffer_size);

const char *sensor_bus_diag_device_name(uint8_t address, const char *source);

bool sensor_bus_diag_record_result(
    sensor_bus_diag_stats_t *stats,
    uint32_t now_ms,
    uint8_t address,
    int channel,
    int err,
    sensor_bus_diag_result_t result,
    int sda,
    int scl,
    const char *source);

bool sensor_bus_diag_summary_capture(
    sensor_bus_diag_stats_t *stats,
    uint32_t now_ms,
    sensor_bus_diag_summary_t *summary);

bool sensor_bus_diag_format_summary_header(
    char *buffer,
    size_t buffer_size,
    uint32_t recovery_attempts,
    uint32_t recoveries,
    uint32_t recovery_failures,
    uint32_t consecutive_failures,
    uint32_t table_overflow,
    int sda,
    int scl);

/* age_ok_ms is UINT32_MAX when no successful transaction has been recorded. */
bool sensor_bus_diag_format_summary_address(
    char *buffer,
    size_t buffer_size,
    const sensor_bus_diag_address_t *address,
    uint32_t now_ms,
    const char *error_name);

size_t sensor_bus_diag_emit_summary(
    const sensor_bus_diag_summary_t *summary,
    uint32_t recovery_attempts,
    uint32_t recoveries,
    uint32_t recovery_failures,
    uint32_t consecutive_failures,
    int sda,
    int scl,
    sensor_bus_diag_error_name_fn error_name,
    sensor_bus_diag_print_fn print,
    void *context);
