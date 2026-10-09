#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT 32U
#define SENSOR_BUS_PROBE_DIAG_BYTES 4U
#define SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES 6144U
#define SENSOR_BUS_PROBE_DIAG_SUMMARY_INTERVAL_SECONDS 60U
#define SENSOR_BUS_PROBE_DIAG_SUMMARY_INTERVAL_US UINT64_C(60000000)

typedef enum {
    SENSOR_BUS_PROBE_PHASE_TCA_CLOSE = 0,
    SENSOR_BUS_PROBE_PHASE_TCA_SELECT,
    SENSOR_BUS_PROBE_PHASE_TCA_VERIFY,
    SENSOR_BUS_PROBE_PHASE_PROBE,
    SENSOR_BUS_PROBE_PHASE_READ,
    SENSOR_BUS_PROBE_PHASE_WRITE,
    SENSOR_BUS_PROBE_PHASE_COUNT,
} sensor_bus_probe_phase_t;

typedef enum {
    SENSOR_BUS_PROBE_RESULT_OK = 0,
    SENSOR_BUS_PROBE_RESULT_NACK,
    SENSOR_BUS_PROBE_RESULT_TIMEOUT,
    SENSOR_BUS_PROBE_RESULT_OTHER,
    SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH,
    SENSOR_BUS_PROBE_RESULT_COUNT,
} sensor_bus_probe_result_t;

typedef struct {
    sensor_bus_probe_phase_t phase;
    uint8_t address;
    int8_t channel;
    uint8_t tx_len;
    uint8_t rx_len;
    uint8_t tx[SENSOR_BUS_PROBE_DIAG_BYTES];
    uint8_t rx[SENSOR_BUS_PROBE_DIAG_BYTES];
    bool expected_valid;
    uint8_t expected;
    bool readback_mismatch;
    sensor_bus_probe_result_t result;
    int32_t err;
    uint64_t elapsed_us;
    int8_t sda_before;
    int8_t scl_before;
    int8_t sda_after;
    int8_t scl_after;
} sensor_bus_probe_diag_transaction_t;

/* A key packs phase (3 bits), address (7 bits), and channel (4 bits). */
typedef struct {
    uint16_t key;
    uint8_t last_result;
    uint8_t flags;
    uint16_t counts[SENSOR_BUS_PROBE_RESULT_COUNT];
    uint32_t sample_count;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t last_summary_seconds;
    uint32_t last_summary_fraction_us;
    int32_t last_err;
} sensor_bus_probe_diag_group_t;

typedef struct {
    sensor_bus_probe_diag_group_t groups[SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT];
    uint32_t pass_index;
    uint32_t groups_dropped;
    bool pass_active;
    bool pass_full;
    bool idle_valid;
    int8_t idle_sda;
    int8_t idle_scl;
} sensor_bus_probe_diag_policy_t;

/* One object owns all diagnostics RAM, including transaction and format scratch. */
typedef struct {
    sensor_bus_probe_diag_policy_t policy;
    sensor_bus_probe_diag_transaction_t transaction;
    sensor_bus_probe_diag_group_t summary;
    char tx_hex[SENSOR_BUS_PROBE_DIAG_BYTES * 2U + 1U];
    char rx_hex[SENSOR_BUS_PROBE_DIAG_BYTES * 2U + 1U];
    char expected_hex[3U];
    bool sda_before;
    bool scl_before;
    bool sda_after;
    bool scl_after;
    bool readback_mismatch;
    bool changed;
    bool should_print;
    int64_t started_us;
    int64_t finished_us;
    uint64_t completed_us;
} sensor_bus_probe_diag_storage_t;

void sensor_bus_probe_diag_policy_reset(sensor_bus_probe_diag_policy_t *state);

void sensor_bus_probe_diag_policy_begin_pass(
    sensor_bus_probe_diag_policy_t *state,
    uint32_t pass_index,
    uint32_t full_passes);

/* Returns false for invalid input; overflowed groups are accepted but untracked. */
bool sensor_bus_probe_diag_policy_record(
    sensor_bus_probe_diag_policy_t *state,
    const sensor_bus_probe_diag_transaction_t *transaction,
    uint64_t now_us,
    bool *changed,
    bool *should_print);

/* Returns and resets the saturating dropped-group count for the summary line. */
uint32_t sensor_bus_probe_diag_policy_end_pass(
    sensor_bus_probe_diag_policy_t *state);

bool sensor_bus_probe_diag_policy_idle_changed(
    sensor_bus_probe_diag_policy_t *state,
    int sda,
    int scl);

bool sensor_bus_probe_diag_policy_take_summary(
    sensor_bus_probe_diag_policy_t *state,
    size_t group_index,
    uint64_t now_us,
    sensor_bus_probe_diag_group_t *summary);

sensor_bus_probe_phase_t sensor_bus_probe_diag_group_phase(
    const sensor_bus_probe_diag_group_t *group);
uint8_t sensor_bus_probe_diag_group_address(
    const sensor_bus_probe_diag_group_t *group);
int8_t sensor_bus_probe_diag_group_channel(
    const sensor_bus_probe_diag_group_t *group);

const char *sensor_bus_probe_diag_phase_name(sensor_bus_probe_phase_t phase);

bool sensor_bus_probe_diag_format_hex(
    char *buffer,
    size_t buffer_size,
    const uint8_t *bytes,
    size_t byte_count);

uint32_t sensor_bus_probe_diag_saturating_add_u32(uint32_t left, uint32_t right);
