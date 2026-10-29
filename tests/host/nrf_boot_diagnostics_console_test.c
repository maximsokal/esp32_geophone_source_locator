#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"

static nrf_boot_twis_record_ring_t s_load_ring;
static bool s_load_test;
static unsigned s_pops;
static unsigned s_pending;
static unsigned s_prints;
static bool s_refilling;
static int64_t s_now_ms;
static char s_last_line[512];

int64_t k_uptime_get(void) { return s_now_ms; }

void nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(nrf_bootstrap_twis_diagnostics_t *diagnostics)
{
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->diagnostic_ring_dropped_total = s_load_ring.dropped_total;
}

bool nrf_bootstrap_runtime_zephyr_drain_twis_record(struct nrf_boot_twis_record *record)
{
    if (s_load_test) {
        ++s_pops;
        return nrf_boot_twis_record_ring_pop(&s_load_ring, record);
    }
    if (!s_refilling && s_pending == 0U) return false;
    assert(s_pops < NRF_BOOT_TRANSACTION_CONSOLE_DRAIN_LIMIT);
    ++s_pops;
    if (s_pending != 0U) --s_pending;
    *record = (nrf_boot_twis_record_t){
        .index = 100U + s_pops,
        .length = 38U,
        .direction = NRF_BOOT_TWIS_DIRECTION_TX_PREPARED,
        .prefix_length = 4U,
        .prefix = {'G', 'S', 1U, 0x82U},
        .tx_prepared = true,
    };
    return true;
}

int printk(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(s_last_line, sizeof(s_last_line), format, args);
    va_end(args);
    assert(written > 0);
    ++s_prints;
    s_now_ms += 25; /* UART transmission gives the producer time to refill. */
    return written;
}

int main(void)
{
    s_refilling = true;
    nrf_boot_diagnostics_console_drain();
    assert(s_pops == NRF_BOOT_TRANSACTION_CONSOLE_DRAIN_LIMIT);
    assert(s_prints == 0U);
    /* Returning lets the production main loop service health and sleep, so
     * its lower-priority assignment queue can run despite continuous input. */
    s_refilling = false;
    s_pending = NRF_BOOT_TRANSACTION_CONSOLE_DRAIN_LIMIT + 1U;
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pending == 1U);
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pending == 0U && s_pops == 1U);
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pops == 0U);
    /* 200 clean hardware records/s, 50 ms main-loop cadence. Neither UART
     * printing nor a four-record consumer may let routine detail fill the ring. */
    s_load_test = true;
    s_now_ms = 10000;
    const unsigned prints_before = s_prints;
    for (unsigned tick = 0U; tick < 200U; ++tick) {
        for (unsigned i = 0U; i < 10U; ++i) {
            const nrf_boot_twis_record_t record = {
                .index = 1000U + tick * 10U + i,
                .direction = NRF_BOOT_TWIS_DIRECTION_WRITE_DONE,
                .rx_amount = 12U,
            };
            (void)nrf_boot_twis_record_ring_push(&s_load_ring, &record);
        }
        s_pops = 0U;
        nrf_boot_diagnostics_console_drain();
        s_now_ms = 10000 + (int64_t)(tick + 1U) * 50;
    }
    assert(s_load_ring.dropped_total == 0U);
    assert(s_load_ring.write_index == s_load_ring.read_index);
    assert(s_prints - prints_before <= 10U);
    /* A fault storm must keep bounded error detail and summaries without
     * starving the consumer or hiding overflow behind a reset counter. */
    const unsigned fault_prints_before = s_prints;
    s_now_ms = 20000;
    for (unsigned tick = 0U; tick < 100U; ++tick) {
        for (unsigned i = 0U; i < 10U; ++i) {
            const nrf_boot_twis_record_t record = {
                .index = 3000U + tick * 10U + i,
                .direction = NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR,
                .result = -EPROTO,
            };
            (void)nrf_boot_twis_record_ring_push(&s_load_ring, &record);
        }
        s_pops = 0U;
        nrf_boot_diagnostics_console_drain();
        s_now_ms = 20000 + (int64_t)(tick + 1U) * 50;
    }
    assert(s_load_ring.dropped_total == 0U);
    assert(s_load_ring.write_index == s_load_ring.read_index);
    assert(s_prints - fault_prints_before <= 25U);
    /* Initial detail is preserved, but printing it is still bounded per pass. */
    for (unsigned i = 0U; i < 10U; ++i) {
        const nrf_boot_twis_record_t record = {
            .index = i + 1U, .direction = NRF_BOOT_TWIS_DIRECTION_WRITE_DONE,
        };
        assert(nrf_boot_twis_record_ring_push(&s_load_ring, &record));
    }
    s_pops = 0U;
    const unsigned detail_prints_before = s_prints;
    nrf_boot_diagnostics_console_drain();
#if CONFIG_APP_NRF_VERBOSE_LOG
    assert(s_prints - detail_prints_before == 4U);
    assert(s_load_ring.write_index - s_load_ring.read_index == 6U);
#else
    assert(s_prints - detail_prints_before == 1U);
    assert(s_load_ring.write_index == s_load_ring.read_index);
#endif
    puts("nrf_boot_diagnostics_console: PASS");
    return 0;
}
