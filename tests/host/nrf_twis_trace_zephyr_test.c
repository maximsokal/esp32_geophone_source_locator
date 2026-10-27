#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nrfx_twis.h>
#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"

int __wrap_nrfx_twis_init(nrfx_twis_t *instance, const nrfx_twis_config_t *config,
                         nrfx_twis_event_handler_t handler);
int __wrap_nrfx_twis_tx_prepare(nrfx_twis_t *instance, const void *buffer, size_t length);

NRF_TWIS_Type test_upstream_registers;
static nrfx_twis_t s_upstream = {&test_upstream_registers};
static nrfx_twis_event_handler_t s_target_handler;
static nrfx_twis_event_handler_t s_other_handler;
static nrf_boot_twis_record_t s_records[32];
static unsigned s_record_count;
static unsigned s_forwarded;
static int s_init_result;
static int s_prepare_result;
static bool s_supply_buffer = true;
static uint32_t s_cycles;
static const uint8_t s_frame[38] = {0x47, 0x53, 0x01, 0x82, 38, 0};
static uint8_t s_rx_buffer[16];

uint32_t k_cycle_get_32(void) { s_cycles += 17U; return s_cycles; }

void nrf_bootstrap_runtime_zephyr_record_twis_hardware(const struct nrf_boot_twis_record *record)
{
    assert(s_record_count < sizeof(s_records) / sizeof(s_records[0]));
    s_records[s_record_count++] = *record;
}

int __real_nrfx_twis_init(nrfx_twis_t *instance, const nrfx_twis_config_t *config,
                         nrfx_twis_event_handler_t handler)
{
    (void)config;
    if (s_init_result == 0) {
        if (instance == &s_upstream) s_target_handler = handler;
        else s_other_handler = handler;
    }
    return s_init_result;
}

int __real_nrfx_twis_tx_prepare(nrfx_twis_t *instance, const void *buffer, size_t length)
{
    (void)instance;
    assert(buffer == s_frame && length == sizeof(s_frame));
    return s_prepare_result;
}

static void original_handler(const nrfx_twis_event_t *event)
{
    ++s_forwarded;
    if (event->type == NRFX_TWIS_EVT_WRITE_DONE) {
        /* The shim callback and the next READ_REQ reuse this buffer; the trace must have sampled it already. */
        memset(s_rx_buffer, 0xEE, sizeof(s_rx_buffer));
    }
    if (event->type == NRFX_TWIS_EVT_READ_REQ && s_supply_buffer) {
        assert(__wrap_nrfx_twis_tx_prepare(&s_upstream, s_frame, sizeof(s_frame)) == s_prepare_result);
    }
}

static void emit(nrfx_twis_event_type_t type, uint32_t value)
{
    nrfx_twis_event_t event = {.type = type};
    event.data.error = value;
    s_target_handler(&event);
}

int main(void)
{
    nrfx_twis_config_t config = {0};
    assert(__wrap_nrfx_twis_init(&s_upstream, &config, original_handler) == 0);
    assert(s_target_handler != original_handler);
    emit(NRFX_TWIS_EVT_READ_REQ, 0U);
    assert(s_record_count == 1U && s_forwarded == 1U);
    assert(s_records[0].direction == NRF_BOOT_TWIS_DIRECTION_TX_PREPARED);
    assert(s_records[0].length == 38U && s_records[0].prefix[3] == 0x82U);
    assert(s_records[0].tx_prepared && s_records[0].prepare_result == 0);
    assert(s_records[0].prepare_us == 17U);

    test_upstream_registers.tx_amount = 31U;
    emit(NRFX_TWIS_EVT_READ_DONE, 99U);
    assert(s_records[1].direction == NRF_BOOT_TWIS_DIRECTION_READ_DONE);
    assert(s_records[1].tx_amount == 31U && s_records[1].hw_error == 0U);
    assert(s_records[1].length == 38U && s_records[1].tx_prepared);

    test_upstream_registers.tx_amount = 32U;
    emit(NRFX_TWIS_EVT_READ_ERROR, 4U);
    assert(s_records[2].direction == NRF_BOOT_TWIS_DIRECTION_READ_ERROR);
    assert(s_records[2].tx_amount == 32U && s_records[2].hw_error == 4U);
    assert(s_records[2].result == -EIO);
    emit(NRFX_TWIS_EVT_GENERAL_ERROR, 256U);
    assert(s_records[3].direction == NRF_BOOT_TWIS_DIRECTION_GENERAL_ERROR);
    assert(s_records[3].hw_error == 256U && s_records[3].prefix_length == 0U);

    s_prepare_result = -EINVAL;
    emit(NRFX_TWIS_EVT_READ_REQ, 0U);
    assert(!s_records[4].tx_prepared && s_records[4].prepare_result == -EINVAL);
    assert(s_records[4].length == 38U && s_records[4].result == -EINVAL);
    s_supply_buffer = false;
    emit(NRFX_TWIS_EVT_READ_REQ, 0U);
    assert(!s_records[5].tx_prepared && s_records[5].length == 0U);
    assert(s_records[5].prepare_result == -ENOSYS);

    const unsigned before_write = s_record_count;
    emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
    emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
    assert(s_record_count == before_write && s_forwarded == 8U);

    NRF_TWIS_Type other_registers = {0};
    nrfx_twis_t other = {&other_registers};
    assert(__wrap_nrfx_twis_init(&other, &config, original_handler) == 0);
    assert(s_other_handler == original_handler);
    assert(__wrap_nrfx_twis_tx_prepare(&other, s_frame, sizeof(s_frame)) == -EINVAL);
    assert(s_record_count == before_write);
    s_init_result = -EBUSY;
    assert(__wrap_nrfx_twis_init(&s_upstream, &config, original_handler) == -EBUSY);
    s_supply_buffer = true;
    s_prepare_result = 0;
    emit(NRFX_TWIS_EVT_READ_REQ, 0U);
    assert(s_records[s_record_count - 1U].tx_prepared);

    /* Receive side: a 12 byte write is sampled before the app callback touches the buffer. */
    const unsigned before_rx = s_record_count;
    const uint8_t request[12] = {0x47, 0x53, 0x01, 0x07, 0x0c, 0x00, 0x81, 0x00, 1, 2, 3, 4};
    memcpy(s_rx_buffer, request, sizeof(request));
    test_upstream_registers.rx_buffer = s_rx_buffer;
    test_upstream_registers.rx_amount = 12U;
    emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
    emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
    assert(s_record_count == before_rx + 1U);
    assert(s_records[before_rx].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_DONE);
    assert(s_records[before_rx].rx_amount == 12U && s_records[before_rx].result == 0);
    assert(s_records[before_rx].prefix_length == NRF_BOOT_TRANSACTION_PREFIX_CAPACITY);
    assert(memcmp(s_records[before_rx].prefix, request, NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) == 0);
    assert(s_records[before_rx].event_dt_us == 17U);
    assert(s_rx_buffer[0] == 0xEE); /* the callback did run and clobber the buffer afterwards */

    /* A short write keeps only the bytes that were received. */
    memcpy(s_rx_buffer, request, sizeof(request));
    test_upstream_registers.rx_amount = 3U;
    emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
    emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
    assert(s_records[before_rx + 1U].prefix_length == 3U && s_records[before_rx + 1U].rx_amount == 3U);

    /* An error is recorded with the ERRORSRC value even when nothing was received. */
    test_upstream_registers.rx_amount = 0U;
    emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
    emit(NRFX_TWIS_EVT_WRITE_ERROR, 1U);
    assert(s_record_count == before_rx + 3U);
    assert(s_records[before_rx + 2U].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
    assert(s_records[before_rx + 2U].hw_error == 1U && s_records[before_rx + 2U].rx_amount == 0U);
    assert(s_records[before_rx + 2U].result == -EIO && s_records[before_rx + 2U].prefix_length == 0U);

    puts("nRF TWIS hardware trace: PASS");
    return 0;
}
