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
int __wrap_nrfx_twis_rx_prepare(nrfx_twis_t *instance, void *buffer, size_t length);
void __wrap_nrfx_twis_irq_handler(nrfx_twis_t *instance);

NRF_TWIS_Type test_upstream_registers;
static nrfx_twis_t s_upstream = {&test_upstream_registers};
static nrfx_twis_event_handler_t s_target_handler;
static nrfx_twis_event_handler_t s_other_handler;
static nrf_boot_twis_record_t s_records[64];
static unsigned s_record_count;
static unsigned s_forwarded;
static int s_init_result;
static int s_prepare_result;
static int s_rx_prepare_result;
static unsigned s_rx_prepare_calls;
static bool s_supply_buffer = true;
static bool s_supply_rx_buffer = true;
static uint32_t s_cycles;
static const uint8_t s_frame[38] = {0x47, 0x53, 0x01, 0x82, 38, 0};
static uint8_t s_rx_buffer[16];
static bool s_generate_rx_started = true;
static const nrfx_twis_event_t *s_irq_event;

void __real_nrfx_twis_irq_handler(nrfx_twis_t *instance)
{
    /* nrfx clears the hardware event before calling its event handler. */
    instance->p_reg->rx_started = false;
    if (s_irq_event != NULL) s_target_handler(s_irq_event);
}

uint32_t k_cycle_get_32(void) { s_cycles += 17U; return s_cycles; }

void nrf_bootstrap_runtime_zephyr_begin_twis_write(void) {}

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

int __real_nrfx_twis_rx_prepare(nrfx_twis_t *instance, void *buffer, size_t length)
{
    assert(instance == &s_upstream);
    assert(buffer == s_rx_buffer && length == sizeof(s_rx_buffer));
    ++s_rx_prepare_calls;
    if (s_rx_prepare_result == 0) instance->p_reg->rx_buffer = buffer;
    return s_rx_prepare_result;
}

static void original_handler(const nrfx_twis_event_t *event)
{
    ++s_forwarded;
    if (event->type == NRFX_TWIS_EVT_WRITE_DONE) {
        /* The shim callback and the next READ_REQ reuse this buffer; the trace must have sampled it already. */
        memset(s_rx_buffer, 0xEE, sizeof(s_rx_buffer));
    }
    if (event->type == NRFX_TWIS_EVT_WRITE_REQ && s_supply_rx_buffer) {
        assert(__wrap_nrfx_twis_rx_prepare(&s_upstream, s_rx_buffer, sizeof(s_rx_buffer))
               == s_rx_prepare_result);
    }
    if (event->type == NRFX_TWIS_EVT_READ_REQ && s_supply_buffer) {
        assert(__wrap_nrfx_twis_tx_prepare(&s_upstream, s_frame, sizeof(s_frame)) == s_prepare_result);
    }
}

static void emit(nrfx_twis_event_type_t type, uint32_t value)
{
    nrfx_twis_event_t event = {.type = type};
    event.data.error = value;
    if (type == NRFX_TWIS_EVT_WRITE_DONE || type == NRFX_TWIS_EVT_WRITE_ERROR) {
        test_upstream_registers.rx_started = s_generate_rx_started;
    }
    s_irq_event = &event;
    __wrap_nrfx_twis_irq_handler(&s_upstream);
    s_irq_event = NULL;
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

    /* A WRITE_DONE without a matching WRITE_REQ has no prepared RX DMA
     * transaction. The shim must not dispatch stale buffer contents as a
     * fresh protocol frame (seen as apparent 12-byte / 10us writes). */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = 12U;
        memset(s_rx_buffer, 0, 12U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before].result == -EPROTO);
    }

    /* A transition to a read ends any unfinished RX attempt. A delayed
     * WRITE_DONE afterwards must not be dispatched with the old DMA count. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        s_supply_buffer = true;
        test_upstream_registers.rx_amount = 12U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
        emit(NRFX_TWIS_EVT_READ_REQ, 0U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 2U);
        assert(s_record_count == records_before + 2U);
        assert(s_records[records_before + 1U].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before + 1U].result == -EPROTO);
    }

    /* A WRITE_REQ exists but nrfx refused to arm EasyDMA. A later completion
     * must not forward the stale RX buffer to the application's ASSIGN decoder. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        const unsigned prepared_before = s_rx_prepare_calls;
        s_rx_prepare_result = -EACCES;
        test_upstream_registers.rx_amount = 12U;
        memset(s_rx_buffer, 0, 12U);
        emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_rx_prepare_calls == prepared_before + 1U);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before].result == -EACCES);
        s_rx_prepare_result = 0;
    }

    /* No DMA preparation at all must also prevent stale data dispatch. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        const unsigned prepared_before = s_rx_prepare_calls;
        s_supply_rx_buffer = false;
        test_upstream_registers.rx_amount = 12U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_rx_prepare_calls == prepared_before);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before].result == -ENOSYS);
        s_supply_rx_buffer = true;
    }

    /* Recovery: a new successful DMA arm must allow the next valid frame. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = 12U;
        memcpy(s_rx_buffer, s_frame, 12U);
        emit(NRFX_TWIS_EVT_WRITE_REQ, 0U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 2U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_DONE);
        assert(s_records[records_before].result == 0);
    }

    /* Successfully arming DMA is not evidence that an address-only write
     * replaced the previous AMOUNT. Reproduce the captured 12 bytes / 11 us. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = 12U;
        memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U); /* buf_req: DMA has not started */
        s_cycles -= 6U; /* next cycle read is 11 us after WRITE_REQ */
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        /* Rejected, but classified apart from bus errors: nothing failed on the wire. */
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_STALE_AMOUNT);
        assert(s_records[records_before].result == -EPROTO);
        assert(s_records[records_before].hw_error == 0U);
        assert(s_records[records_before].rx_amount == 12U);
        assert(s_records[records_before].event_dt_us == 11U);
        assert(s_records[records_before].prefix_length == 0U);
    }

    /* The same stale amount together with a hardware error is a real fault. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = 12U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        s_cycles -= 6U;
        emit(NRFX_TWIS_EVT_WRITE_ERROR, 1U);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before].hw_error == 1U);
    }

    /* Real payload durations at the requested bus rates, plus fast-mode
     * and 1 MHz, must still dispatch the exact bytes after a rejected write. */
    const uint32_t rates[] = {25000U, 50000U, 100000U, 400000U, 1000000U};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        const uint32_t duration_us = (12U * 9000000U + rates[i] - 1U) / rates[i];
        memcpy(s_rx_buffer, request, sizeof(request));
        test_upstream_registers.rx_amount = 12U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        s_cycles += duration_us - 17U;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 2U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_DONE);
        assert(s_records[records_before].event_dt_us == duration_us);
        assert(memcmp(s_records[records_before].prefix, request,
                      NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) == 0);
    }

    /* Even a slow completion cannot make AMOUNT larger than the armed buffer. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = sizeof(s_rx_buffer) + 1U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        s_cycles += 10000U;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].result == -EPROTO);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        assert(s_records[records_before].prefix_length == 0U);
    }

    /* First-byte buffering and rounded timestamps must not reject legal
     * transfers. Count validation applies even when RX started before IRQ. */
    const struct { uint32_t amount; uint32_t elapsed; bool buf_req; bool valid; } edges[] = {
        {1U, 0U, true, true},
        {12U, 86U, true, false},
        {12U, 87U, true, true},
        {12U, 88U, true, true},
        {12U, 11U, false, true},
        {16U, 2000U, true, true},
        {17U, 2000U, false, false},
        {UINT32_MAX, 2000U, true, false},
    };
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        memset(s_rx_buffer, 0, sizeof(s_rx_buffer)); /* zeros are legal payload */
        test_upstream_registers.rx_amount = edges[i].amount;
        s_cycles = UINT32_MAX - 20U; /* also cross the cycle-counter wrap */
        emit(NRFX_TWIS_EVT_WRITE_REQ, edges[i].buf_req ? 1U : 0U);
        s_cycles = s_cycles - 17U + edges[i].elapsed;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + (edges[i].valid ? 2U : 1U));
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].result == (edges[i].valid ? 0 : -EPROTO));
        if (!edges[i].valid) {
            assert(s_records[records_before].direction ==
                   (edges[i].amount > sizeof(s_rx_buffer)
                       ? NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR
                       : NRF_BOOT_TWIS_DIRECTION_WRITE_STALE_AMOUNT));
        }
        assert(s_records[records_before].event_dt_us == edges[i].elapsed);
        assert(s_records[records_before].prefix_length ==
               (edges[i].valid ? (edges[i].amount < NRF_BOOT_TRANSACTION_PREFIX_CAPACITY
                   ? edges[i].amount : NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) : 0U));
    }

    /* A plausible duration cannot validate stale AMOUNT without RXSTARTED. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        s_generate_rx_started = false;
        test_upstream_registers.rx_amount = 12U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        s_cycles += 10000U;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 1U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].result == -EPROTO);
        assert(s_records[records_before].direction == NRF_BOOT_TWIS_DIRECTION_WRITE_STALE_AMOUNT);
        assert(s_records[records_before].prefix_length == 0U);
        s_generate_rx_started = true;
    }

    /* RXSTARTED may have its own IRQ long before STOP; the driver clears
     * the register in that IRQ, but the completion must retain the proof. */
    {
        const unsigned forwarded_before = s_forwarded;
        const unsigned records_before = s_record_count;
        test_upstream_registers.rx_amount = 12U;
        memcpy(s_rx_buffer, request, sizeof(request));
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        test_upstream_registers.rx_started = true;
        __wrap_nrfx_twis_irq_handler(&s_upstream);
        assert(!test_upstream_registers.rx_started);
        s_cycles += 2000U;
        s_generate_rx_started = false;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 2U);
        assert(s_record_count == records_before + 1U);
        assert(s_records[records_before].result == 0);
        assert(memcmp(s_records[records_before].prefix, request,
                      NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) == 0);

        /* Neither that old READY nor an unrelated instance's READY can
         * validate the next transfer's cached count. */
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        other_registers.rx_started = true;
        __wrap_nrfx_twis_irq_handler(&other);
        assert(!other_registers.rx_started);
        s_cycles += 2000U;
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 3U);
        assert(s_record_count == records_before + 2U);
        assert(s_records[records_before + 1U].result == -EPROTO);

        /* Zero count and no RXSTARTED is an ordinary address-only probe. */
        test_upstream_registers.rx_amount = 0U;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        emit(NRFX_TWIS_EVT_WRITE_DONE, 0U);
        assert(s_forwarded == forwarded_before + 4U);
        assert(s_record_count == records_before + 2U);
        s_generate_rx_started = true;
    }

    /* Hardware ERRORSRC must survive every software rejection branch. */
    {
        const unsigned records_before = s_record_count;
        emit(NRFX_TWIS_EVT_WRITE_ERROR, 4U); /* no WRITE_REQ */
        assert(s_records[records_before].result == -EPROTO);
        assert(s_records[records_before].hw_error == 4U);
        s_rx_prepare_result = -EACCES;
        emit(NRFX_TWIS_EVT_WRITE_REQ, 1U);
        emit(NRFX_TWIS_EVT_WRITE_ERROR, 2U);
        assert(s_records[records_before + 1U].result == -EACCES);
        assert(s_records[records_before + 1U].hw_error == 2U);
        s_rx_prepare_result = 0;
    }

    puts("nRF TWIS hardware trace: PASS");
    return 0;
}
