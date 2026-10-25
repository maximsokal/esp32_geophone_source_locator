#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/hwinfo.h>

#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_watchdog_zephyr.h"

static uint32_t s_cycle;
static uint32_t s_last_cycle;
static int64_t s_uptime = 100U;

ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    static const uint8_t id[] = {0x78U, 0x56U, 0x34U, 0x12U};
    size_t copied = length < sizeof(id) ? length : sizeof(id);

    memcpy(buffer, id, copied);
    return (ssize_t)copied;
}

int sys_csrand_get(void *destination, size_t length)
{
    (void)destination;
    (void)length;
    return -EIO;
}

int64_t k_uptime_get(void)
{
    return s_uptime++;
}

uint32_t k_cycle_get_32(void)
{
    s_last_cycle = ++s_cycle;
    return s_last_cycle;
}

uint32_t nrf_watchdog_zephyr_boot_reset_cause(void)
{
    return 0x00000080U;
}

static void test_led_event_counters(void)
{
    nrf_log_runtime_zephyr_led_counters_t counters = {0};

    assert(nrf_log_runtime_zephyr_get_led_counters(&counters) == NRF_LOG_ERROR_OK);
    assert(counters.twis_protocol_error_total == 0U);
    assert(counters.cs_ranging_failure_total == 0U);
    assert(counters.watchdog_participant_stale_total == 0U);
    assert(counters.role_lost_total == 0U);

    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME, 1U, 2U);
    assert(nrf_log_runtime_zephyr_get_led_counters(&counters) == NRF_LOG_ERROR_OK);
    assert(counters.twis_protocol_error_total == 1U);

    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_TIMEOUT, 1U, 2U);
    assert(nrf_log_runtime_zephyr_get_led_counters(&counters) == NRF_LOG_ERROR_OK);
    assert(counters.cs_ranging_failure_total == 1U);

    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE, 1U, 2U);
    assert(nrf_log_runtime_zephyr_get_led_counters(&counters) == NRF_LOG_ERROR_OK);
    assert(counters.watchdog_participant_stale_total == 1U);

    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_ROLE_LOST, 1U, 2U);
    assert(nrf_log_runtime_zephyr_get_led_counters(&counters) == NRF_LOG_ERROR_OK);
    assert(counters.role_lost_total == 1U);
}

int main(void)
{
    nrf_log_status_t status;
    nrf_log_block_t block;
    nrf_log_read_request_t request;
    uint8_t status_frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    uint8_t block_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t status_length = 0U;
    size_t block_length = 0U;
    uint32_t old_boot_id;

    assert(nrf_log_runtime_zephyr_init() == 0);
    assert(nrf_log_runtime_zephyr_build_status(
               7U,
               status_frame,
               sizeof(status_frame),
               &status_length) == NRF_LOG_ERROR_OK);
    assert(status_length == NRF_LOG_PROTOCOL_STATUS_LENGTH);
    assert(nrf_log_protocol_decode_status(
               status_frame,
               status_length,
               &status) == NRF_LOG_ERROR_OK);
    assert(status.boot_id != 0U);
    assert((status.flags & NRF_LOG_STATUS_FLAG_BOOT_ID_FALLBACK) != 0U);
    assert(status.boot_id == (0x12345678U ^ s_last_cycle));
    old_boot_id = status.boot_id;
    assert((status.flags & NRF_LOG_STATUS_FLAG_FIRMWARE_INFO) != 0U);
    {
        uint8_t firmware_frame[NRF_LOG_PROTOCOL_FIRMWARE_LENGTH];
        size_t firmware_length = 0U;
        nrf_log_firmware_info_t firmware = {0};
        assert(nrf_log_runtime_zephyr_build_firmware(123U, firmware_frame,
            sizeof(firmware_frame), &firmware_length) == NRF_LOG_ERROR_OK);
        assert(nrf_log_protocol_decode_firmware(firmware_frame, firmware_length,
            &firmware) == NRF_LOG_ERROR_OK);
        assert(firmware.request_id == 123U && firmware.boot_id == status.boot_id);
        assert(firmware.flags == NRF_LOG_FIRMWARE_FLAG_DIRTY);
        assert(firmware.git_commit[0] == 0x00U && firmware.git_commit[1] == 0x11U);
        assert(firmware.git_commit[19] == 0x67U);
        assert(nrf_log_runtime_zephyr_build_firmware(124U, firmware_frame,
            sizeof(firmware_frame) - 1U, &firmware_length) == NRF_LOG_ERROR_BAD_LENGTH);
    }

    request = (nrf_log_read_request_t){
        .request_id = 8U,
        .expected_boot_id = status.boot_id,
        .cursor_seq = 0U,
        .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
    };
    assert(nrf_log_runtime_zephyr_build_block(
               &request,
               block_frame,
               sizeof(block_frame),
               &block_length) == NRF_LOG_ERROR_OK);
    assert(nrf_log_runtime_zephyr_max_block_build_cycles() != 0U);
    assert(nrf_log_protocol_decode_block(
               block_frame,
               block_length,
               NULL,
               &block) == NRF_LOG_ERROR_OK);
    assert(block.record_count == 1U);
    assert(block.records[0].event_id == NRF_LOG_EVENT_BOOT);
    assert(block.records[0].arg0 == 0x00000080U);
    assert(block.records[0].arg1 == 1U);

    assert(nrf_log_runtime_zephyr_test_set_next_seq(NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_LOG_TEST_EVENT, 1U, 2U);
    assert(nrf_log_runtime_zephyr_build_status(
               9U,
               status_frame,
               sizeof(status_frame),
               &status_length) == NRF_LOG_ERROR_OK);
    assert(nrf_log_protocol_decode_status(
               status_frame,
               status_length,
               &status) == NRF_LOG_ERROR_OK);
    assert(status.boot_id != 0U && status.boot_id != old_boot_id);

    request.request_id = 10U;
    request.expected_boot_id = status.boot_id;
    request.cursor_seq = 0U;
    assert(nrf_log_runtime_zephyr_build_block(
               &request,
               block_frame,
               sizeof(block_frame),
               &block_length) == NRF_LOG_ERROR_OK);
    assert(nrf_log_protocol_decode_block(
               block_frame,
               block_length,
               NULL,
               &block) == NRF_LOG_ERROR_OK);
    assert(block.record_count == 2U);
    assert(block.records[0].event_id == NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER);
    assert(block.records[0].arg0 == old_boot_id);
    assert(block.records[0].arg1 == status.boot_id);
    assert(block.records[1].event_id == NRF_LOG_EVENT_LOG_TEST_EVENT);
    test_led_event_counters();
    return 0;
}
