#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"

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
}

bool nrf_bootstrap_runtime_zephyr_drain_twis_record(struct nrf_boot_twis_record *record)
{
    if (!s_refilling && s_pending == 0U) return false;
    assert(s_pops < NRF_BOOT_TRANSACTION_CONSOLE_BATCH_LIMIT);
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
    assert(s_pops == NRF_BOOT_TRANSACTION_CONSOLE_BATCH_LIMIT);
#if CONFIG_APP_NRF_VERBOSE_LOG
    assert(s_prints == NRF_BOOT_TRANSACTION_CONSOLE_BATCH_LIMIT);
    assert(strstr(s_last_line, "type=0x82") != NULL);
#else
    assert(s_prints == 0U);
#endif
    /* Returning lets the production main loop service health and sleep, so
     * its lower-priority assignment queue can run despite continuous input. */
    s_refilling = false;
    s_pending = NRF_BOOT_TRANSACTION_CONSOLE_BATCH_LIMIT + 1U;
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pending == 1U);
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pending == 0U && s_pops == 1U);
    s_pops = 0U;
    nrf_boot_diagnostics_console_drain();
    assert(s_pops == 0U);
    puts("nrf_boot_diagnostics_console: PASS");
    return 0;
}
