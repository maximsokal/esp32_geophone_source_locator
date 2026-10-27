#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_boot_diagnostics.h"

#if defined(NRF_BOOT_DIAGNOSTICS_TEST_HOOKS)
static bool s_twis_spsc_hook_called;
static bool s_twis_spsc_published_early;
static bool s_twis_spsc_hook_armed;
static nrf_boot_twis_record_t s_twis_spsc_interleaved_output;

void nrf_boot_twis_record_ring_test_after_first_field(nrf_boot_twis_record_ring_t *ring)
{
    if (s_twis_spsc_hook_armed) {
        s_twis_spsc_hook_called = true;
        s_twis_spsc_published_early = nrf_boot_twis_record_ring_pop(
            ring,
            &s_twis_spsc_interleaved_output);
        s_twis_spsc_hook_armed = false;
    }
}
#endif

typedef struct {
    nrf_boot_lamp_t set_lamp[8];
    bool set_on[8];
    bool pin_on[NRF_BOOT_LAMP_COUNT];
    bool unavailable[NRF_BOOT_LAMP_COUNT];
    int fail_on_result[NRF_BOOT_LAMP_COUNT];
    size_t set_count;
    uint32_t sleep_count;
    uint32_t elapsed_ms;
    uint32_t on_during_sleep;
} lamp_fake_t;

static int fake_set_lamp(void *context, nrf_boot_lamp_t lamp, bool on)
{
    lamp_fake_t *fake = (lamp_fake_t *)context;
    assert(fake != NULL);
    assert((unsigned int)lamp < (unsigned int)NRF_BOOT_LAMP_COUNT);
    assert(fake->set_count < (sizeof(fake->set_lamp) / sizeof(fake->set_lamp[0])));

    fake->set_lamp[fake->set_count] = lamp;
    fake->set_on[fake->set_count] = on;
    ++fake->set_count;
    if (on && fake->unavailable[lamp]) {
        return NRF_BOOT_LAMP_UNAVAILABLE;
    }
    if (on && fake->fail_on_result[lamp] != 0) {
        return fake->fail_on_result[lamp];
    }
    fake->pin_on[lamp] = on;
    return 0;
}

static void fake_sleep(void *context, uint32_t duration_ms)
{
    lamp_fake_t *fake = (lamp_fake_t *)context;
    uint32_t active_count = 0U;

    assert(fake != NULL);
    assert(duration_ms == NRF_BOOT_LAMP_TEST_DURATION_MS);
    for (size_t index = 0U; index < (size_t)NRF_BOOT_LAMP_COUNT; ++index) {
        if (fake->pin_on[index]) {
            ++active_count;
        }
    }
    assert(active_count == 1U);
    ++fake->sleep_count;
    fake->on_during_sleep += active_count;
    fake->elapsed_ms += duration_ms;
}

static void test_lamp_test_order_timing_and_off_state(void)
{
    static const nrf_boot_lamp_t expected_order[NRF_BOOT_LAMP_COUNT] = {
        NRF_BOOT_LAMP_BLUE,
        NRF_BOOT_LAMP_GREEN,
        NRF_BOOT_LAMP_YELLOW,
        NRF_BOOT_LAMP_RED,
    };
    lamp_fake_t fake = {0};
    nrf_boot_lamp_result_t results[NRF_BOOT_LAMP_COUNT];
    nrf_boot_lamp_summary_t summary;

    assert(nrf_boot_lamp_test_run(
               fake_set_lamp, fake_sleep, &fake, results, &summary) == 0);
    assert(summary.tested_count == (uint32_t)NRF_BOOT_LAMP_COUNT);
    assert(summary.unavailable_count == 0U);
    assert(summary.error_count == 0U);
    assert(fake.elapsed_ms == 1200U);
    assert(fake.sleep_count == (uint32_t)NRF_BOOT_LAMP_COUNT);
    assert(fake.on_during_sleep == (uint32_t)NRF_BOOT_LAMP_COUNT);
    assert(fake.set_count == 8U);

    for (size_t index = 0U; index < (size_t)NRF_BOOT_LAMP_COUNT; ++index) {
        assert(results[index].available);
        assert(results[index].on_result == 0);
        assert(results[index].off_result == 0);
        assert(fake.set_lamp[index * 2U] == expected_order[index]);
        assert(fake.set_on[index * 2U]);
        assert(fake.set_lamp[index * 2U + 1U] == expected_order[index]);
        assert(!fake.set_on[index * 2U + 1U]);
        assert(!fake.pin_on[index]);
    }
}

static void test_lamp_test_skips_unavailable_and_reports_gpio_errors(void)
{
    lamp_fake_t fake = {0};
    nrf_boot_lamp_result_t results[NRF_BOOT_LAMP_COUNT];
    nrf_boot_lamp_summary_t summary;

    fake.unavailable[NRF_BOOT_LAMP_GREEN] = true;
    fake.fail_on_result[NRF_BOOT_LAMP_RED] = -EIO;
    assert(nrf_boot_lamp_test_run(
               fake_set_lamp, fake_sleep, &fake, results, &summary) == 0);

    assert(summary.tested_count == 3U);
    assert(summary.unavailable_count == 1U);
    assert(summary.error_count == 1U);
    assert(fake.elapsed_ms == 600U);
    assert(fake.sleep_count == 2U);
    assert(!results[NRF_BOOT_LAMP_GREEN].available);
    assert(results[NRF_BOOT_LAMP_GREEN].on_result == NRF_BOOT_LAMP_UNAVAILABLE);
    assert(results[NRF_BOOT_LAMP_RED].available);
    assert(results[NRF_BOOT_LAMP_RED].on_result == -EIO);
    assert(results[NRF_BOOT_LAMP_RED].off_result == 0);
    assert(!fake.pin_on[NRF_BOOT_LAMP_BLUE]);
    assert(!fake.pin_on[NRF_BOOT_LAMP_YELLOW]);
    assert(!fake.pin_on[NRF_BOOT_LAMP_RED]);
}

static void test_status_format_is_deterministic_and_bounded(void)
{
    nrf_boot_status_t status = {0};
    char line[NRF_BOOT_STATUS_LINE_CAPACITY];
    char guarded[10];
    const unsigned char guard_sentinel = 0xA5U;
    int written;

    status.uptime_ms = 5000U;
    status.lifecycle = NRF_NODE_LIFECYCLE_READY;
    status.role = NRF_NODE_ROLE_UNASSIGNED;
    status.blue_led_state = 0;
    status.logger_led_output.severity_latched = NRF_LOGGER_LED_SEVERITY_NONE;
    status.logger_led_output.reason_source = NRF_LOGGER_LED_SOURCE_NONE;
    written = nrf_boot_status_format(&status, line, sizeof(line));
    assert(written > 0);
    assert((size_t)written < sizeof(line));
    assert(strcmp(
               line,
               "status: up=5000ms lifecycle=ready role=unassigned "
               "twis={addr_match=n/a,write_requested=n/a,write_received=0,write_bytes=0,read_requested=0,stop=n/a,callback_err=0,last_type=n/a,last_count=0,ring_dropped=0} "
               "sensors={imu=0/init=0,env=0/init=0} led={g=0,y=0,r=0,sev=none/source=none,blue=off} "
               "logs={overwritten=0,lost=0} led_counters={gpio_fail=0,policy_invalid=0,policy_time=0,blue_fail=0}") == 0);

    status.uptime_ms = UINT64_MAX;
    status.lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
    status.role = NRF_NODE_ROLE_REMOTE;
    status.write_received_count = UINT32_MAX;
    status.write_received_bytes = UINT32_MAX;
    status.read_requested_count = UINT32_MAX;
    status.callback_error_count = UINT32_MAX;
    status.last_request_type_valid = true;
    status.last_request_type = UINT8_MAX;
    status.last_request_type_count = UINT32_MAX;
    status.imu_ready = true;
    status.imu_init_result = INT32_MIN;
    status.env_ready = true;
    status.env_init_result = INT32_MAX;
    status.logger_led_output.green = true;
    status.logger_led_output.yellow = true;
    status.logger_led_output.red = true;
    status.logger_led_output.severity_latched = NRF_LOGGER_LED_SEVERITY_FATAL;
    status.logger_led_output.reason_source = NRF_LOGGER_LED_SOURCE_INVALID_INPUT;
    status.blue_led_state = 1;
    status.log_overwritten_total = UINT32_MAX;
    status.log_lost_internal_total = UINT32_MAX;
    status.logger_gpio_failure_count = UINT32_MAX;
    status.logger_policy_invalid_count = UINT32_MAX;
    status.logger_policy_timestamp_anomaly_count = UINT32_MAX;
    status.blue_led_failure_count = UINT32_MAX;
    status.diagnostic_ring_dropped_total = UINT32_MAX;
    written = nrf_boot_status_format(&status, line, sizeof(line));
    assert(written > 0);
    assert((size_t)written < sizeof(line));
    assert(strstr(line, "18446744073709551615") != NULL);
    assert(strstr(line, "write_received=4294967295") != NULL);
    assert(strstr(line, "init=-2147483648") != NULL);
    assert(strstr(line, "sev=fatal/source=invalid_input,blue=on") != NULL);
    assert(strstr(line, "ring_dropped=4294967295") != NULL);
    assert(memchr(line, '\0', sizeof(line)) != NULL);

    memset(guarded, 0xA5, sizeof(guarded));
    written = nrf_boot_status_format(&status, &guarded[1], 8U);
    assert(written == -ENOSPC);
    assert(memcmp(&guarded[0], &guard_sentinel, sizeof(guard_sentinel)) == 0);
    assert(memcmp(&guarded[9], &guard_sentinel, sizeof(guard_sentinel)) == 0);
    assert(guarded[8] == '\0');
}

static void test_transaction_ring_preserves_records_and_counts_overflow(void)
{
    nrf_boot_twis_record_ring_t ring = {0};
    nrf_boot_twis_record_t input = {0};
    nrf_boot_twis_record_t output;

    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_RING_CAPACITY; ++index) {
        input.index = index + 1U;
        input.length = index + 10U;
        input.direction = (uint8_t)(index & 1U);
        input.prefix_length = NRF_BOOT_TRANSACTION_PREFIX_CAPACITY;
        input.prefix[0] = (uint8_t)index;
        input.result = -(int32_t)index;
        input.tx_amount = index;
        input.hw_error = index + 20U;
        input.prepare_us = index + 30U;
        input.prepare_result = -(int32_t)(index + 40U);
        input.tx_prepared = (index & 1U) != 0U;
        assert(nrf_boot_twis_record_ring_push(&ring, &input));
    }

    input.index = NRF_BOOT_TRANSACTION_RING_CAPACITY + 1U;
    assert(!nrf_boot_twis_record_ring_push(&ring, &input));
    assert(ring.dropped_total == 1U);

    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_RING_CAPACITY; ++index) {
        assert(nrf_boot_twis_record_ring_pop(&ring, &output));
        assert(output.index == index + 1U);
        assert(output.length == index + 10U);
        assert(output.direction == (uint8_t)(index & 1U));
        assert(output.prefix_length == NRF_BOOT_TRANSACTION_PREFIX_CAPACITY);
        assert(output.prefix[0] == (uint8_t)index);
        assert(output.result == -(int32_t)index);
        assert(output.tx_amount == index);
        assert(output.hw_error == index + 20U);
        assert(output.prepare_us == index + 30U);
        assert(output.prepare_result == -(int32_t)(index + 40U));
        assert(output.tx_prepared == ((index & 1U) != 0U));
    }
    assert(!nrf_boot_twis_record_ring_pop(&ring, &output));

    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_RING_CAPACITY; ++index) {
        input.index = index + 100U;
        assert(nrf_boot_twis_record_ring_push(&ring, &input));
    }
    ring.dropped_total = UINT32_MAX;
    assert(!nrf_boot_twis_record_ring_push(&ring, &input));
    assert(ring.dropped_total == UINT32_MAX);
}

static void test_twis_spsc_commit_interleave(void)
{
#if defined(NRF_BOOT_DIAGNOSTICS_TEST_HOOKS)
    nrf_boot_twis_record_ring_t ring = {0};
    nrf_boot_twis_record_t input = {
        .index = 0x10203040U,
        .length = 0x50607080U,
        .direction = NRF_BOOT_TWIS_DIRECTION_WRITE_RECEIVED,
        .prefix_length = 3U,
        .prefix = {0xA1U, 0xB2U, 0xC3U},
        .result = -EIO,
    };
    nrf_boot_twis_record_t output;

    s_twis_spsc_hook_called = false;
    s_twis_spsc_published_early = false;
    s_twis_spsc_hook_armed = true;
    assert(nrf_boot_twis_record_ring_push(&ring, &input));
    assert(s_twis_spsc_hook_called);
    assert(!s_twis_spsc_published_early);
    assert(nrf_boot_twis_record_ring_pop(&ring, &output));
    assert(output.index == input.index);
    assert(output.length == input.length);
    assert(output.direction == input.direction);
    assert(output.prefix_length == input.prefix_length);
    assert(memcmp(output.prefix, input.prefix, sizeof(input.prefix)) == 0);
    assert(output.result == input.result);
    assert(!nrf_boot_twis_record_ring_pop(&ring, &output));
#endif
}

static void test_transaction_rate_limit_details_summaries_and_errors(void)
{
    nrf_boot_twis_log_state_t state = {0};
    nrf_boot_twis_record_t record = {0};
    char summary[NRF_BOOT_TRANSACTION_LINE_CAPACITY];

    for (uint32_t index = 1U; index <= NRF_BOOT_TRANSACTION_DETAIL_LIMIT; ++index) {
        record.index = index;
        assert(nrf_boot_twis_log_decide(&state, &record, 0U) ==
               NRF_BOOT_TWIS_LOG_DETAIL);
    }

    record.index = NRF_BOOT_TRANSACTION_DETAIL_LIMIT + 1U;
    assert(nrf_boot_twis_log_decide(&state, &record, 0U) == NRF_BOOT_TWIS_LOG_NONE);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 999U) == NRF_BOOT_TWIS_LOG_NONE);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 1000U) ==
           NRF_BOOT_TWIS_LOG_SUMMARY);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 1000U) == NRF_BOOT_TWIS_LOG_NONE);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 1999U) == NRF_BOOT_TWIS_LOG_NONE);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 2000U) ==
           NRF_BOOT_TWIS_LOG_SUMMARY);

    record.result = -EIO;
    for (uint32_t index = 0U; index < NRF_BOOT_TRANSACTION_ERROR_LINES_PER_SECOND; ++index) {
        record.index++;
        assert(nrf_boot_twis_log_decide(&state, &record, 2001U) ==
               NRF_BOOT_TWIS_LOG_ERROR);
    }
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 2001U) ==
           NRF_BOOT_TWIS_LOG_ERROR_SUPPRESSED);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 2999U) ==
           NRF_BOOT_TWIS_LOG_ERROR_SUPPRESSED);
    record.index++;
    assert(nrf_boot_twis_log_decide(&state, &record, 3000U) ==
           NRF_BOOT_TWIS_LOG_ERROR);
    assert(state.suppressed_error_count == 2U);
    assert(nrf_boot_twis_summary_format(&state, 7U, summary, sizeof(summary)) > 0);
    assert(strstr(summary, "suppressed=2 ring_dropped=7") != NULL);
}

static void test_transaction_format_is_bounded_with_maximum_values(void)
{
    nrf_boot_twis_record_t record = {0};
    char line[NRF_BOOT_TRANSACTION_LINE_CAPACITY];
    char guarded[34];
    const unsigned char sentinel = 0xA5U;

    record.index = UINT32_MAX;
    record.length = UINT32_MAX;
    record.direction = NRF_BOOT_TWIS_DIRECTION_READ_REQUESTED;
    record.prefix_length = UINT8_MAX;
    record.prefix[0] = 0x00U;
    record.prefix[1] = 0x11U;
    record.prefix[2] = 0x22U;
    record.prefix[3] = 0x33U;
    record.prefix[4] = 0x44U;
    record.prefix[5] = 0x55U;
    record.prefix[6] = 0x66U;
    record.prefix[7] = 0xFFU;
    record.result = INT32_MIN;

    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "tx=4294967295 dir=read_requested len=4294967295") != NULL);
    assert(strstr(line, "bytes=00 11 22 33 44 55 66 ff result=-2147483648") != NULL);
    memset(guarded, sentinel, sizeof(guarded));
    assert(nrf_boot_twis_record_format(&record, guarded, 32U) == -ENOSPC);
    assert(guarded[31] == '\0');
    assert((unsigned char)guarded[32] == sentinel);
    assert((unsigned char)guarded[33] == sentinel);
}

static void test_change_detector_reports_each_state_family_once(void)
{
    nrf_boot_change_detector_t detector = {0};
    nrf_boot_change_snapshot_t snapshot = {0};

    snapshot.lifecycle = (uint8_t)NRF_NODE_LIFECYCLE_BOOTING;
    snapshot.role = (uint8_t)NRF_NODE_ROLE_UNASSIGNED;
    snapshot.blue_led_state = -1;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_ALL);
    assert(nrf_boot_change_detector_update(&detector, &snapshot) == 0U);
    snapshot.sensor_diagnostics_valid = true;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           (NRF_BOOT_CHANGE_SENSOR_RESULT | NRF_BOOT_CHANGE_LOGGER_OUTPUT |
            NRF_BOOT_CHANGE_LOGGER_LATCH | NRF_BOOT_CHANGE_ORIENTATION));

    snapshot.lifecycle = (uint8_t)NRF_NODE_LIFECYCLE_READY;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_LIFECYCLE);
    snapshot.role = (uint8_t)NRF_NODE_ROLE_REMOTE;
    snapshot.node_id = 23U;
    snapshot.role_assignment_sequence = 1U;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_ROLE_ASSIGNMENT);
    snapshot.sensor_activation_sequence = 1U;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_SENSOR_ACTIVATION);
    snapshot.sensor_activation_complete_sequence = 1U;
    snapshot.imu_ready = true;
    snapshot.imu_init_result = -EIO;
    snapshot.env_ready = true;
    snapshot.env_init_result = INT32_MAX;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           (NRF_BOOT_CHANGE_SENSOR_ACTIVATION | NRF_BOOT_CHANGE_SENSOR_RESULT));
    snapshot.blue_led_state = 1;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_BLUE_LED);
    snapshot.logger_green = true;
    snapshot.logger_yellow = true;
    snapshot.logger_red = true;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_LOGGER_OUTPUT);
    snapshot.logger_severity = (uint8_t)NRF_LOGGER_LED_SEVERITY_FATAL;
    snapshot.logger_reason_source = (uint8_t)NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_LOGGER_LATCH);
    snapshot.orientation_state = (uint8_t)NRF_ORIENTATION_STATE_ACCEPTED;
    snapshot.verticality_state = (uint8_t)NRF_VERTICALITY_ACCEPTED;
    assert(nrf_boot_change_detector_update(&detector, &snapshot) ==
           NRF_BOOT_CHANGE_ORIENTATION);
    assert(nrf_boot_change_detector_update(&detector, &snapshot) == 0U);
}

static void test_hardware_trace_format_and_default_logging(void)
{
    nrf_boot_twis_record_t record = {
        .index = 1000U,
        .length = 38U,
        .direction = NRF_BOOT_TWIS_DIRECTION_READ_DONE,
        .prefix_length = 6U,
        .prefix = {0x47U, 0x53U, 0x01U, 0x82U, 38U, 0U},
        .tx_amount = 33U,
        .prepare_us = 17U,
        .tx_prepared = true,
    };
    nrf_boot_twis_log_state_t state = {0};
    char line[NRF_BOOT_TRANSACTION_LINE_CAPACITY];
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "event=READ_DONE type=0x82 selected_len=38 prepared=1") != NULL);
    assert(strstr(line, "prepare_ret=0 prepare_us=17 tx_amount=33") != NULL);
    assert(strstr(line, "hw_error=0x00000000 READ_ERROR=0") != NULL);
    assert(nrf_boot_twis_log_decide(&state, &record, 0U) == NRF_BOOT_TWIS_LOG_DETAIL);
    assert(nrf_boot_twis_log_decide(&state, &record, 1U) == NRF_BOOT_TWIS_LOG_DETAIL);
    record.direction = NRF_BOOT_TWIS_DIRECTION_TX_PREPARED;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "tx_amount=n/a") != NULL);
    assert(nrf_boot_twis_log_decide(&state, &record, 2U) == NRF_BOOT_TWIS_LOG_DETAIL);
    record.direction = NRF_BOOT_TWIS_DIRECTION_READ_ERROR;
    record.hw_error = 4U;
    record.result = -EIO;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "event=READ_ERROR") != NULL);
    assert(strstr(line, "hw_error=0x00000004 READ_ERROR=1") != NULL);
    assert(nrf_boot_twis_log_decide(&state, &record, 3U) == NRF_BOOT_TWIS_LOG_ERROR);
    record.tx_prepared = false;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "tx_amount=n/a") != NULL);
    record.index = UINT32_MAX;
    record.length = UINT32_MAX;
    record.tx_amount = UINT32_MAX;
    record.hw_error = UINT32_MAX;
    record.prepare_us = UINT32_MAX;
    record.prepare_result = INT32_MIN;
    record.result = INT32_MIN;
    record.tx_prepared = true;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "tx_amount=4294967295") != NULL);
    assert(nrf_boot_twis_record_format(&record, line, 16U) == -ENOSPC);
}

static void test_receive_side_records_format_ring_and_log_policy(void)
{
    nrf_boot_twis_record_t record = {
        .index = 61U,
        .direction = NRF_BOOT_TWIS_DIRECTION_WRITE_DONE,
        .prefix_length = 8U,
        .prefix = {0x47U, 0x53U, 0x01U, 0x03U, 0x20U, 0x00U, 0x93U, 0xAEU},
        .rx_amount = 19U,
        .event_dt_us = 412U,
    };
    nrf_boot_twis_record_t popped = {0};
    nrf_boot_twis_record_ring_t ring = {0};
    nrf_boot_twis_log_state_t state = {0};
    char line[NRF_BOOT_TRANSACTION_LINE_CAPACITY];

    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "twis_rx: tx=61 event=WRITE_DONE rx_amount=19 dt_us=412") != NULL);
    assert(strstr(line, "hw_error=0x00000000 WRITE_ERROR=0 result=0 bytes=47 53 01 03 20 00 93 ae") != NULL);

    /* The new fields survive the SPSC ring. */
    assert(nrf_boot_twis_record_ring_push(&ring, &record));
    assert(nrf_boot_twis_record_ring_pop(&ring, &popped));
    assert(popped.rx_amount == 19U && popped.event_dt_us == 412U);
    assert(popped.direction == NRF_BOOT_TWIS_DIRECTION_WRITE_DONE);
    assert(memcmp(popped.prefix, record.prefix, sizeof(record.prefix)) == 0);

    /* Past the detail limit a clean receive stays visible, an error is rate limited like others. */
    record.index = NRF_BOOT_TRANSACTION_DETAIL_LIMIT + 5U;
    assert(nrf_boot_twis_log_decide(&state, &record, 10U) == NRF_BOOT_TWIS_LOG_DETAIL);
    record.direction = NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR;
    record.hw_error = 1U;
    record.result = -EIO;
    record.prefix_length = 0U;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "event=WRITE_ERROR") != NULL);
    assert(strstr(line, "hw_error=0x00000001 WRITE_ERROR=1 result=-5 bytes=-") != NULL);
    assert(nrf_boot_twis_log_decide(&state, &record, 11U) == NRF_BOOT_TWIS_LOG_ERROR);

    /* Largest values still fit, and a small buffer is reported instead of overrun. */
    record.index = UINT32_MAX;
    record.rx_amount = UINT32_MAX;
    record.event_dt_us = UINT32_MAX;
    record.hw_error = UINT32_MAX;
    record.result = INT32_MIN;
    record.prefix_length = 8U;
    assert(nrf_boot_twis_record_format(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "rx_amount=4294967295 dt_us=4294967295") != NULL);
    assert(nrf_boot_twis_record_format(&record, line, 20U) == -ENOSPC);
    assert(nrf_boot_twis_record_format(&record, line, 120U) == -ENOSPC);
}

int main(void)
{
    test_lamp_test_order_timing_and_off_state();
    test_lamp_test_skips_unavailable_and_reports_gpio_errors();
    test_status_format_is_deterministic_and_bounded();
    test_transaction_ring_preserves_records_and_counts_overflow();
    test_twis_spsc_commit_interleave();
    test_transaction_rate_limit_details_summaries_and_errors();
    test_transaction_format_is_bounded_with_maximum_values();
    test_hardware_trace_format_and_default_logging();
    test_receive_side_records_format_ring_and_log_policy();
    test_change_detector_reports_each_state_family_once();
    return 0;
}
