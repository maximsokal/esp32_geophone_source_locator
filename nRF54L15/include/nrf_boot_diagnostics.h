#ifndef NRF_BOOT_DIAGNOSTICS_H
#define NRF_BOOT_DIAGNOSTICS_H

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nrf_logger_led.h"
#include "nrf_node_state.h"
#include "nrf_orientation.h"
#include "nrf_verticality_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_BOOT_LAMP_TEST_DURATION_MS 300U
#define NRF_BOOT_STATUS_LINE_CAPACITY 640U
#define NRF_BOOT_TRANSACTION_LINE_CAPACITY 256U
#define NRF_BOOT_TRANSACTION_RING_CAPACITY 64U
#define NRF_BOOT_TRANSACTION_DETAIL_LIMIT 64U
#define NRF_BOOT_TRANSACTION_SUMMARY_PERIOD_MS 1000U
#define NRF_BOOT_TRANSACTION_ERROR_LINES_PER_SECOND 4U
#define NRF_BOOT_TRANSACTION_PREFIX_CAPACITY 8U

typedef enum {
    NRF_BOOT_LAMP_BLUE = 0,
    NRF_BOOT_LAMP_GREEN,
    NRF_BOOT_LAMP_YELLOW,
    NRF_BOOT_LAMP_RED,
    NRF_BOOT_LAMP_COUNT,
} nrf_boot_lamp_t;

/* The LED adapters return this value when the corresponding pin is absent. */
#define NRF_BOOT_LAMP_UNAVAILABLE 1

typedef struct {
    bool available;
    int on_result;
    int off_result;
} nrf_boot_lamp_result_t;

typedef struct {
    uint32_t tested_count;
    uint32_t unavailable_count;
    uint32_t error_count;
} nrf_boot_lamp_summary_t;

typedef int (*nrf_boot_lamp_set_fn)(
    void *context,
    nrf_boot_lamp_t lamp,
    bool on);
typedef void (*nrf_boot_lamp_sleep_fn)(void *context, uint32_t duration_ms);

typedef struct {
    uint64_t uptime_ms;
    nrf_node_lifecycle_t lifecycle;
    nrf_node_role_t role;
    uint32_t write_received_count;
    uint32_t write_received_bytes;
    uint32_t read_requested_count;
    uint32_t callback_error_count;
    bool last_request_type_valid;
    uint8_t last_request_type;
    uint32_t last_request_type_count;
    bool imu_ready;
    int32_t imu_init_result;
    bool env_ready;
    int32_t env_init_result;
    nrf_logger_led_output_t logger_led_output;
    int8_t blue_led_state;
    uint32_t log_overwritten_total;
    uint32_t log_lost_internal_total;
    uint32_t logger_gpio_failure_count;
    uint32_t logger_policy_invalid_count;
    uint32_t logger_policy_timestamp_anomaly_count;
    uint32_t blue_led_failure_count;
    uint32_t diagnostic_ring_dropped_total;
} nrf_boot_status_t;

typedef enum {
    NRF_BOOT_TWIS_DIRECTION_WRITE_RECEIVED = 0,
    NRF_BOOT_TWIS_DIRECTION_READ_REQUESTED,
    NRF_BOOT_TWIS_DIRECTION_TX_PREPARED,
    NRF_BOOT_TWIS_DIRECTION_READ_DONE,
    NRF_BOOT_TWIS_DIRECTION_READ_ERROR,
    NRF_BOOT_TWIS_DIRECTION_GENERAL_ERROR,
} nrf_boot_twis_direction_t;

/* Callback records contain no pointers and are copied through the SPSC ring. */
typedef struct nrf_boot_twis_record {
    uint32_t index;
    uint32_t length;
    uint8_t direction;
    uint8_t prefix_length;
    uint8_t prefix[NRF_BOOT_TRANSACTION_PREFIX_CAPACITY];
    int32_t result;
    uint32_t tx_amount;
    uint32_t hw_error;
    uint32_t prepare_us;
    int32_t prepare_result;
    bool tx_prepared;
} nrf_boot_twis_record_t;

typedef struct {
    volatile uint32_t index;
    volatile uint32_t length;
    volatile uint8_t direction;
    volatile uint8_t prefix_length;
    volatile uint8_t prefix[NRF_BOOT_TRANSACTION_PREFIX_CAPACITY];
    volatile int32_t result;
    volatile uint32_t tx_amount;
    volatile uint32_t hw_error;
    volatile uint32_t prepare_us;
    volatile int32_t prepare_result;
    volatile bool tx_prepared;
} nrf_boot_twis_volatile_record_t;

/* Single producer (serialized TWIS buffer callbacks), single main-loop reader. */
typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t dropped_total;
    nrf_boot_twis_volatile_record_t records[NRF_BOOT_TRANSACTION_RING_CAPACITY];
} nrf_boot_twis_record_ring_t;

typedef struct {
    bool summary_clock_started;
    uint64_t last_summary_ms;
    uint64_t error_window_second;
    uint32_t error_lines_in_window;
    uint32_t post_detail_write_count;
    uint32_t post_detail_read_count;
    uint32_t post_detail_error_count;
    uint32_t suppressed_error_count;
} nrf_boot_twis_log_state_t;

typedef enum {
    NRF_BOOT_TWIS_LOG_NONE = 0,
    NRF_BOOT_TWIS_LOG_DETAIL,
    NRF_BOOT_TWIS_LOG_ERROR,
    NRF_BOOT_TWIS_LOG_SUMMARY,
    NRF_BOOT_TWIS_LOG_ERROR_SUPPRESSED,
} nrf_boot_twis_log_action_t;

enum {
    NRF_BOOT_CHANGE_LIFECYCLE = UINT32_C(1) << 0,
    NRF_BOOT_CHANGE_ROLE_ASSIGNMENT = UINT32_C(1) << 1,
    NRF_BOOT_CHANGE_SENSOR_ACTIVATION = UINT32_C(1) << 2,
    NRF_BOOT_CHANGE_SENSOR_RESULT = UINT32_C(1) << 3,
    NRF_BOOT_CHANGE_BLUE_LED = UINT32_C(1) << 4,
    NRF_BOOT_CHANGE_LOGGER_OUTPUT = UINT32_C(1) << 5,
    NRF_BOOT_CHANGE_LOGGER_LATCH = UINT32_C(1) << 6,
    NRF_BOOT_CHANGE_ORIENTATION = UINT32_C(1) << 7,
    NRF_BOOT_CHANGE_ALL = (UINT32_C(1) << 8) - 1U,
};

typedef struct {
    uint8_t lifecycle;
    uint8_t role;
    uint32_t node_id;
    uint32_t role_assignment_sequence;
    uint32_t sensor_activation_sequence;
    uint32_t sensor_activation_complete_sequence;
    bool sensor_diagnostics_valid;
    bool imu_ready;
    int32_t imu_init_result;
    bool env_ready;
    int32_t env_init_result;
    int8_t blue_led_state;
    bool logger_green;
    bool logger_yellow;
    bool logger_red;
    uint8_t logger_severity;
    uint8_t logger_reason_source;
    uint8_t orientation_state;
    uint8_t verticality_state;
} nrf_boot_change_snapshot_t;

typedef struct {
    bool initialized;
    nrf_boot_change_snapshot_t last;
} nrf_boot_change_detector_t;

static inline void nrf_boot_saturating_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

#if defined(NRF_BOOT_DIAGNOSTICS_TEST_HOOKS)
void nrf_boot_twis_record_ring_test_after_first_field(nrf_boot_twis_record_ring_t *ring);
#endif

static inline bool nrf_boot_twis_record_ring_push(
    nrf_boot_twis_record_ring_t *ring,
    const nrf_boot_twis_record_t *record)
{
    uint32_t write_index;
    uint32_t read_index;
    nrf_boot_twis_volatile_record_t *slot;
    uint8_t prefix_length;

    if (ring == NULL || record == NULL) {
        return false;
    }

    write_index = ring->write_index;
    read_index = ring->read_index;
    if (write_index - read_index >= NRF_BOOT_TRANSACTION_RING_CAPACITY) {
        if (ring->dropped_total != UINT32_MAX) {
            ++ring->dropped_total;
        }
        return false;
    }

    slot = &ring->records[write_index % NRF_BOOT_TRANSACTION_RING_CAPACITY];
    prefix_length = record->prefix_length;
    if ((uint32_t)prefix_length > NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) {
        prefix_length = NRF_BOOT_TRANSACTION_PREFIX_CAPACITY;
    }
    slot->index = record->index;
#if defined(NRF_BOOT_DIAGNOSTICS_TEST_HOOKS)
    nrf_boot_twis_record_ring_test_after_first_field(ring);
#endif
    slot->length = record->length;
    slot->direction = record->direction;
    slot->prefix_length = prefix_length;
    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_PREFIX_CAPACITY; ++index) {
        slot->prefix[index] = index < prefix_length ? record->prefix[index] : 0U;
    }
    slot->result = record->result;
    slot->tx_amount = record->tx_amount;
    slot->hw_error = record->hw_error;
    slot->prepare_us = record->prepare_us;
    slot->prepare_result = record->prepare_result;
    slot->tx_prepared = record->tx_prepared;
    ring->write_index = write_index + 1U;
    return true;
}

static inline bool nrf_boot_twis_record_ring_pop(
    nrf_boot_twis_record_ring_t *ring,
    nrf_boot_twis_record_t *record)
{
    uint32_t read_index;
    nrf_boot_twis_volatile_record_t *slot;

    if (ring == NULL || record == NULL) {
        return false;
    }

    read_index = ring->read_index;
    if (read_index == ring->write_index) {
        return false;
    }

    slot = &ring->records[read_index % NRF_BOOT_TRANSACTION_RING_CAPACITY];
    record->index = slot->index;
    record->length = slot->length;
    record->direction = slot->direction;
    record->prefix_length = slot->prefix_length;
    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_PREFIX_CAPACITY; ++index) {
        record->prefix[index] = slot->prefix[index];
    }
    record->result = slot->result;
    record->tx_amount = slot->tx_amount;
    record->hw_error = slot->hw_error;
    record->prepare_us = slot->prepare_us;
    record->prepare_result = slot->prepare_result;
    record->tx_prepared = slot->tx_prepared;
    ring->read_index = read_index + 1U;
    return true;
}

static inline nrf_boot_twis_log_action_t nrf_boot_twis_log_decide(
    nrf_boot_twis_log_state_t *state,
    const nrf_boot_twis_record_t *record,
    uint64_t now_ms)
{
    const uint64_t error_second = now_ms / 1000U;

    if (state == NULL || record == NULL) {
        return NRF_BOOT_TWIS_LOG_NONE;
    }
    if (record->index <= NRF_BOOT_TRANSACTION_DETAIL_LIMIT) {
        return NRF_BOOT_TWIS_LOG_DETAIL;
    }
    if (record->direction >= NRF_BOOT_TWIS_DIRECTION_TX_PREPARED && record->result == 0) {
        return NRF_BOOT_TWIS_LOG_DETAIL;
    }

    if (!state->summary_clock_started) {
        state->summary_clock_started = true;
        state->last_summary_ms = now_ms;
        state->error_window_second = error_second;
    }
    if (record->direction != NRF_BOOT_TWIS_DIRECTION_WRITE_RECEIVED) {
        nrf_boot_saturating_increment(&state->post_detail_read_count);
    } else {
        nrf_boot_saturating_increment(&state->post_detail_write_count);
    }

    if (record->result != 0) {
        nrf_boot_saturating_increment(&state->post_detail_error_count);
        if (state->error_window_second != error_second) {
            state->error_window_second = error_second;
            state->error_lines_in_window = 0U;
        }
        if (state->error_lines_in_window <
            NRF_BOOT_TRANSACTION_ERROR_LINES_PER_SECOND) {
            ++state->error_lines_in_window;
            return NRF_BOOT_TWIS_LOG_ERROR;
        }
        nrf_boot_saturating_increment(&state->suppressed_error_count);
        if (now_ms >= state->last_summary_ms &&
            now_ms - state->last_summary_ms >=
                NRF_BOOT_TRANSACTION_SUMMARY_PERIOD_MS) {
            state->last_summary_ms = now_ms;
            return NRF_BOOT_TWIS_LOG_SUMMARY;
        }
        return NRF_BOOT_TWIS_LOG_ERROR_SUPPRESSED;
    }

    if (now_ms >= state->last_summary_ms &&
        now_ms - state->last_summary_ms >=
            NRF_BOOT_TRANSACTION_SUMMARY_PERIOD_MS) {
        state->last_summary_ms = now_ms;
        return NRF_BOOT_TWIS_LOG_SUMMARY;
    }

    return NRF_BOOT_TWIS_LOG_NONE;
}

static inline int nrf_boot_twis_record_format(
    const nrf_boot_twis_record_t *record,
    char *buffer,
    size_t capacity)
{
    const char *direction;
    size_t offset;
    uint8_t prefix_length;
    int written;

    if (record == NULL || buffer == NULL || capacity == 0U) {
        return -EINVAL;
    }
    if (record->direction >= NRF_BOOT_TWIS_DIRECTION_TX_PREPARED) {
        const char *event = "GENERAL_ERROR";
        char type[5] = "n/a";
        char amount[11] = "n/a";
        if (record->direction == NRF_BOOT_TWIS_DIRECTION_TX_PREPARED) event = "TX_PREPARED";
        else if (record->direction == NRF_BOOT_TWIS_DIRECTION_READ_DONE) event = "READ_DONE";
        else if (record->direction == NRF_BOOT_TWIS_DIRECTION_READ_ERROR) event = "READ_ERROR";
        if (record->prefix_length >= 4U) {
            (void)snprintf(type, sizeof(type), "0x%02x", (unsigned)record->prefix[3]);
        }
        if (record->tx_prepared &&
            (record->direction == NRF_BOOT_TWIS_DIRECTION_READ_DONE ||
             record->direction == NRF_BOOT_TWIS_DIRECTION_READ_ERROR)) {
            (void)snprintf(amount, sizeof(amount), "%" PRIu32, record->tx_amount);
        }
        written = snprintf(
            buffer, capacity,
            "twis_tx: tx=%" PRIu32 " event=%s type=%s selected_len=%" PRIu32
            " prepared=%u prepare_ret=%" PRId32 " prepare_us=%" PRIu32
            " tx_amount=%s hw_error=0x%08" PRIx32 " READ_ERROR=%u result=%" PRId32,
            record->index, event, type, record->length, (unsigned)record->tx_prepared,
            record->prepare_result, record->prepare_us, amount, record->hw_error,
            (unsigned)(record->direction == NRF_BOOT_TWIS_DIRECTION_READ_ERROR), record->result);
        return written < 0 || (size_t)written >= capacity ? -ENOSPC : written;
    }
    direction = record->direction == NRF_BOOT_TWIS_DIRECTION_READ_REQUESTED
        ? "read_requested"
        : "write_received";
    written = snprintf(
        buffer,
        capacity,
        "twis: tx=%" PRIu32 " dir=%s len=%" PRIu32 " bytes=",
        record->index,
        direction,
        record->length);
    if (written < 0 || (size_t)written >= capacity) {
        return -ENOSPC;
    }
    offset = (size_t)written;
    prefix_length = record->prefix_length;
    if ((uint32_t)prefix_length > NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) {
        prefix_length = NRF_BOOT_TRANSACTION_PREFIX_CAPACITY;
    }
    if (prefix_length == 0U) {
        written = snprintf(buffer + offset, capacity - offset, "-");
        if (written < 0 || (size_t)written >= capacity - offset) {
            return -ENOSPC;
        }
        offset += (size_t)written;
    } else {
        for (uint8_t index = 0U; index < prefix_length; ++index) {
            written = snprintf(
                buffer + offset,
                capacity - offset,
                "%s%02x",
                index == 0U ? "" : " ",
                (unsigned int)record->prefix[index]);
            if (written < 0 || (size_t)written >= capacity - offset) {
                return -ENOSPC;
            }
            offset += (size_t)written;
        }
    }
    written = snprintf(
        buffer + offset,
        capacity - offset,
        " result=%" PRId32,
        record->result);
    if (written < 0 || (size_t)written >= capacity - offset) {
        return -ENOSPC;
    }
    return (int)(offset + (size_t)written);
}

static inline int nrf_boot_twis_summary_format(
    const nrf_boot_twis_log_state_t *state,
    uint32_t dropped_total,
    char *buffer,
    size_t capacity)
{
    int written;

    if (state == NULL || buffer == NULL || capacity == 0U) {
        return -EINVAL;
    }
    written = snprintf(
        buffer,
        capacity,
        "twis: summary write=%" PRIu32 " read=%" PRIu32
        " errors=%" PRIu32 " suppressed=%" PRIu32
        " ring_dropped=%" PRIu32,
        state->post_detail_write_count,
        state->post_detail_read_count,
        state->post_detail_error_count,
        state->suppressed_error_count,
        dropped_total);
    if (written < 0 || (size_t)written >= capacity) {
        return -ENOSPC;
    }
    return written;
}

static inline uint32_t nrf_boot_change_detector_update(
    nrf_boot_change_detector_t *detector,
    const nrf_boot_change_snapshot_t *snapshot)
{
    uint32_t changes = 0U;
    const nrf_boot_change_snapshot_t *last;

    if (detector == NULL || snapshot == NULL) {
        return 0U;
    }
    if (!detector->initialized) {
        detector->last = *snapshot;
        detector->initialized = true;
        return NRF_BOOT_CHANGE_ALL;
    }

    last = &detector->last;
    if (last->lifecycle != snapshot->lifecycle) {
        changes |= NRF_BOOT_CHANGE_LIFECYCLE;
    }
    if (last->role != snapshot->role || last->node_id != snapshot->node_id ||
        last->role_assignment_sequence != snapshot->role_assignment_sequence) {
        changes |= NRF_BOOT_CHANGE_ROLE_ASSIGNMENT;
    }
    if (last->sensor_activation_sequence != snapshot->sensor_activation_sequence ||
        last->sensor_activation_complete_sequence !=
            snapshot->sensor_activation_complete_sequence) {
        changes |= NRF_BOOT_CHANGE_SENSOR_ACTIVATION;
    }
    if (last->sensor_diagnostics_valid != snapshot->sensor_diagnostics_valid) {
        changes |= NRF_BOOT_CHANGE_SENSOR_RESULT |
            NRF_BOOT_CHANGE_LOGGER_OUTPUT |
            NRF_BOOT_CHANGE_LOGGER_LATCH |
            NRF_BOOT_CHANGE_ORIENTATION;
    }
    if (last->imu_ready != snapshot->imu_ready ||
        last->imu_init_result != snapshot->imu_init_result ||
        last->env_ready != snapshot->env_ready ||
        last->env_init_result != snapshot->env_init_result) {
        changes |= NRF_BOOT_CHANGE_SENSOR_RESULT;
    }
    if (last->blue_led_state != snapshot->blue_led_state) {
        changes |= NRF_BOOT_CHANGE_BLUE_LED;
    }
    if (last->logger_green != snapshot->logger_green ||
        last->logger_yellow != snapshot->logger_yellow ||
        last->logger_red != snapshot->logger_red) {
        changes |= NRF_BOOT_CHANGE_LOGGER_OUTPUT;
    }
    if (last->logger_severity != snapshot->logger_severity ||
        last->logger_reason_source != snapshot->logger_reason_source) {
        changes |= NRF_BOOT_CHANGE_LOGGER_LATCH;
    }
    if (last->orientation_state != snapshot->orientation_state ||
        last->verticality_state != snapshot->verticality_state) {
        changes |= NRF_BOOT_CHANGE_ORIENTATION;
    }

    detector->last = *snapshot;
    return changes;
}

static inline const char *nrf_boot_lamp_name(nrf_boot_lamp_t lamp)
{
    switch (lamp) {
    case NRF_BOOT_LAMP_BLUE:
        return "BLUE";
    case NRF_BOOT_LAMP_GREEN:
        return "GREEN";
    case NRF_BOOT_LAMP_YELLOW:
        return "YELLOW";
    case NRF_BOOT_LAMP_RED:
        return "RED";
    default:
        return "UNKNOWN";
    }
}

static inline int nrf_boot_lamp_test_run(
    nrf_boot_lamp_set_fn set_lamp,
    nrf_boot_lamp_sleep_fn sleep_ms,
    void *context,
    nrf_boot_lamp_result_t results[NRF_BOOT_LAMP_COUNT],
    nrf_boot_lamp_summary_t *summary)
{
    static const nrf_boot_lamp_t sequence[NRF_BOOT_LAMP_COUNT] = {
        NRF_BOOT_LAMP_BLUE,
        NRF_BOOT_LAMP_GREEN,
        NRF_BOOT_LAMP_YELLOW,
        NRF_BOOT_LAMP_RED,
    };

    if (set_lamp == NULL || sleep_ms == NULL || results == NULL || summary == NULL) {
        return -EINVAL;
    }

    summary->tested_count = 0U;
    summary->unavailable_count = 0U;
    summary->error_count = 0U;

    for (size_t index = 0U; index < (size_t)NRF_BOOT_LAMP_COUNT; ++index) {
        nrf_boot_lamp_result_t *result = &results[index];
        int set_result;

        result->available = false;
        result->on_result = NRF_BOOT_LAMP_UNAVAILABLE;
        result->off_result = NRF_BOOT_LAMP_UNAVAILABLE;

        set_result = set_lamp(context, sequence[index], true);
        result->on_result = set_result;
        if (set_result == NRF_BOOT_LAMP_UNAVAILABLE) {
            ++summary->unavailable_count;
            continue;
        }

        result->available = true;
        ++summary->tested_count;
        if (set_result != 0) {
            ++summary->error_count;
        } else {
            sleep_ms(context, NRF_BOOT_LAMP_TEST_DURATION_MS);
        }

        set_result = set_lamp(context, sequence[index], false);
        result->off_result = set_result;
        if (set_result != 0) {
            ++summary->error_count;
        }
    }

    return 0;
}

static inline const char *nrf_boot_lifecycle_name(nrf_node_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case NRF_NODE_LIFECYCLE_BOOTING:
        return "booting";
    case NRF_NODE_LIFECYCLE_READY:
        return "ready";
    case NRF_NODE_LIFECYCLE_UNAVAILABLE:
        return "unavailable";
    default:
        return "unknown";
    }
}

static inline const char *nrf_boot_role_name(nrf_node_role_t role)
{
    switch (role) {
    case NRF_NODE_ROLE_UNASSIGNED:
        return "unassigned";
    case NRF_NODE_ROLE_MAIN:
        return "main";
    case NRF_NODE_ROLE_REMOTE:
        return "remote";
    default:
        return "unknown";
    }
}

static inline const char *nrf_boot_severity_name(nrf_logger_led_severity_t severity)
{
    switch (severity) {
    case NRF_LOGGER_LED_SEVERITY_NONE:
        return "none";
    case NRF_LOGGER_LED_SEVERITY_WARNING:
        return "warning";
    case NRF_LOGGER_LED_SEVERITY_ERROR:
        return "error";
    case NRF_LOGGER_LED_SEVERITY_FATAL:
        return "fatal";
    default:
        return "unknown";
    }
}

static inline const char *nrf_boot_blue_led_name(int8_t state)
{
    if (state < 0) {
        return "unavailable";
    }
    return state == 0 ? "off" : "on";
}

static inline const char *nrf_boot_orientation_name(uint8_t state)
{
    switch ((nrf_orientation_state_t)state) {
    case NRF_ORIENTATION_STATE_UNAVAILABLE:
        return "unavailable";
    case NRF_ORIENTATION_STATE_MOVING:
        return "moving";
    case NRF_ORIENTATION_STATE_ACCEPTED:
        return "accepted";
    case NRF_ORIENTATION_STATE_OUTSIDE:
        return "outside";
    default:
        return "unknown";
    }
}

static inline const char *nrf_boot_verticality_name(uint8_t state)
{
    switch ((nrf_verticality_state_t)state) {
    case NRF_VERTICALITY_ACCEPTED:
        return "accepted";
    case NRF_VERTICALITY_STABILIZING:
        return "stabilizing";
    case NRF_VERTICALITY_TILTED:
        return "tilted";
    case NRF_VERTICALITY_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

static inline const char *nrf_boot_logger_source_name(uint8_t source)
{
    switch ((nrf_logger_led_source_t)source) {
    case NRF_LOGGER_LED_SOURCE_BMI160_FAULT:
        return "bmi160";
    case NRF_LOGGER_LED_SOURCE_BME280_FAULT:
        return "bme280";
    case NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS:
        return "log_ring";
    case NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL:
        return "twis";
    case NRF_LOGGER_LED_SOURCE_CS_RANGING:
        return "cs_ranging";
    case NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE:
        return "watchdog";
    case NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL:
        return "boot_init";
    case NRF_LOGGER_LED_SOURCE_ROLE_LOST:
        return "role_lost";
    case NRF_LOGGER_LED_SOURCE_NONE:
        return "none";
    case NRF_LOGGER_LED_SOURCE_INVALID_INPUT:
        return "invalid_input";
    default:
        return "unknown";
    }
}

static inline int nrf_boot_status_format(
    const nrf_boot_status_t *status,
    char *buffer,
    size_t capacity)
{
    int written;

    if (status == NULL || buffer == NULL || capacity == 0U) {
        return -EINVAL;
    }

    if (status->last_request_type_valid) {
        written = snprintf(
            buffer,
            capacity,
            "status: up=%" PRIu64 "ms lifecycle=%s role=%s "
            "twis={addr_match=n/a,write_requested=n/a,write_received=%" PRIu32
            ",write_bytes=%" PRIu32 ",read_requested=%" PRIu32
            ",stop=n/a,callback_err=%" PRIu32 ",last_type=0x%02x,last_count=%" PRIu32
            ",ring_dropped=%" PRIu32 "} "
            "sensors={imu=%u/init=%" PRId32 ",env=%u/init=%" PRId32 "} "
            "led={g=%u,y=%u,r=%u,sev=%s/source=%s,blue=%s} "
            "logs={overwritten=%" PRIu32 ",lost=%" PRIu32 "} "
            "led_counters={gpio_fail=%" PRIu32 ",policy_invalid=%" PRIu32
            ",policy_time=%" PRIu32 ",blue_fail=%" PRIu32 "}",
            status->uptime_ms,
            nrf_boot_lifecycle_name(status->lifecycle),
            nrf_boot_role_name(status->role),
            status->write_received_count,
            status->write_received_bytes,
            status->read_requested_count,
            status->callback_error_count,
            (unsigned int)status->last_request_type,
            status->last_request_type_count,
            status->diagnostic_ring_dropped_total,
            status->imu_ready ? 1U : 0U,
            status->imu_init_result,
            status->env_ready ? 1U : 0U,
            status->env_init_result,
            status->logger_led_output.green ? 1U : 0U,
            status->logger_led_output.yellow ? 1U : 0U,
            status->logger_led_output.red ? 1U : 0U,
            nrf_boot_severity_name(status->logger_led_output.severity_latched),
            nrf_boot_logger_source_name(
                (uint8_t)status->logger_led_output.reason_source),
            nrf_boot_blue_led_name(status->blue_led_state),
            status->log_overwritten_total,
            status->log_lost_internal_total,
            status->logger_gpio_failure_count,
            status->logger_policy_invalid_count,
            status->logger_policy_timestamp_anomaly_count,
            status->blue_led_failure_count);
    } else {
        written = snprintf(
            buffer,
            capacity,
            "status: up=%" PRIu64 "ms lifecycle=%s role=%s "
            "twis={addr_match=n/a,write_requested=n/a,write_received=%" PRIu32
            ",write_bytes=%" PRIu32 ",read_requested=%" PRIu32
            ",stop=n/a,callback_err=%" PRIu32 ",last_type=n/a,last_count=0"
            ",ring_dropped=%" PRIu32 "} "
            "sensors={imu=%u/init=%" PRId32 ",env=%u/init=%" PRId32 "} "
            "led={g=%u,y=%u,r=%u,sev=%s/source=%s,blue=%s} "
            "logs={overwritten=%" PRIu32 ",lost=%" PRIu32 "} "
            "led_counters={gpio_fail=%" PRIu32 ",policy_invalid=%" PRIu32
            ",policy_time=%" PRIu32 ",blue_fail=%" PRIu32 "}",
            status->uptime_ms,
            nrf_boot_lifecycle_name(status->lifecycle),
            nrf_boot_role_name(status->role),
            status->write_received_count,
            status->write_received_bytes,
            status->read_requested_count,
            status->callback_error_count,
            status->diagnostic_ring_dropped_total,
            status->imu_ready ? 1U : 0U,
            status->imu_init_result,
            status->env_ready ? 1U : 0U,
            status->env_init_result,
            status->logger_led_output.green ? 1U : 0U,
            status->logger_led_output.yellow ? 1U : 0U,
            status->logger_led_output.red ? 1U : 0U,
            nrf_boot_severity_name(status->logger_led_output.severity_latched),
            nrf_boot_logger_source_name(
                (uint8_t)status->logger_led_output.reason_source),
            nrf_boot_blue_led_name(status->blue_led_state),
            status->log_overwritten_total,
            status->log_lost_internal_total,
            status->logger_gpio_failure_count,
            status->logger_policy_invalid_count,
            status->logger_policy_timestamp_anomaly_count,
            status->blue_led_failure_count);
    }

    if (written < 0 || (size_t)written >= capacity) {
        return -ENOSPC;
    }
    return written;
}

#ifdef __cplusplus
}
#endif

#endif
