#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

void nrf_boot_diagnostics_console_drain(void)
{
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    static nrf_boot_twis_log_state_t log_state;
    nrf_bootstrap_twis_diagnostics_t diagnostics = {0};
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diagnostics);
#endif
    nrf_boot_twis_record_t record;
    char line[NRF_BOOT_TRANSACTION_LINE_CAPACITY];

    /* An IRQ producer can refill forever; return to main's sleep for ASSIGN/HCI work. */
    for (size_t count = 0U; count < NRF_BOOT_TRANSACTION_CONSOLE_BATCH_LIMIT; ++count) {
        if (!nrf_bootstrap_runtime_zephyr_drain_twis_record(&record)) break;
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        const nrf_boot_twis_log_action_t action = nrf_boot_twis_log_decide(
            &log_state, &record, (uint64_t)k_uptime_get());

        if (record.index == 1U || action == NRF_BOOT_TWIS_LOG_DETAIL ||
            action == NRF_BOOT_TWIS_LOG_ERROR) {
            if (nrf_boot_twis_record_format(&record, line, sizeof(line)) >= 0) {
                printk("%s\n", line);
            }
        } else if (action == NRF_BOOT_TWIS_LOG_SUMMARY) {
            if (nrf_boot_twis_summary_format(
                    &log_state, diagnostics.diagnostic_ring_dropped_total,
                    line, sizeof(line)) >= 0) {
                printk("%s\n", line);
            }
        }
#else
        if (record.index == 1U &&
            nrf_boot_twis_record_format(&record, line, sizeof(line)) >= 0) {
            printk("%s\n", line);
        }
#endif
    }
}
