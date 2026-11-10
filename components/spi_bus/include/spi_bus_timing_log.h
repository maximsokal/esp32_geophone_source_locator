#pragma once

#include <stdbool.h>
#include <stdint.h>


/*
 * When to print the per-operation storage timing lines.
 *
 * Every SD operation used to print four INFO lines (STORAGE_SPI_TIMING and
 * STORAGE_OP_TIMING, begin and end). The console UART is blocking: about
 * 350 characters at 115200 baud keep the calling task busy for roughly 30 ms,
 * and the caller is usually the recorder, the only consumer that protects the
 * realtime ring. The line formats are a HIL contract (tools/analyze_storage_spi.py,
 * tools/storage_spi_contract.ps1), so they are kept and printed for:
 *   - an operation that failed;
 *   - an operation with a notable wait (mutex, TFT quiesce or resume) or duration;
 *   - one sampled operation per summary period, begin and end together.
 * Everything else is only counted. The sampled operation also closes the
 * summary window: its end prints one line with the operation count and the
 * worst values since the previous summary.
 *
 * The state is plain data owned by the caller's lock.
 */
#define APP_SPI_BUS_TIMING_NOTABLE_WAIT_US     50000LL
#define APP_SPI_BUS_TIMING_NOTABLE_ELAPSED_US 200000LL
#define APP_SPI_BUS_TIMING_SUMMARY_PERIOD_US 5000000LL

typedef struct
{
    bool started;                 /* a first operation has been sampled */
    bool sampled_operation;       /* the operation in progress is the periodic sample */
    int64_t last_sample_us;

    uint32_t operations;          /* since the previous summary */
    uint32_t failures;
    int64_t max_mutex_wait_us;
    int64_t max_quiesce_us;
    int64_t max_resume_us;
    int64_t max_elapsed_us;
} app_spi_bus_timing_log_t;

typedef struct
{
    uint32_t operations;
    uint32_t failures;
    int64_t window_us;
    int64_t max_mutex_wait_us;
    int64_t max_quiesce_us;
    int64_t max_resume_us;
    int64_t max_elapsed_us;
} app_spi_bus_timing_summary_t;


static inline int64_t app_spi_bus_timing_max(int64_t current, int64_t value)
{
    return value > current ? value : current;
}


/* Call once per operation at begin. Returns true when the begin line is to be printed. */
static inline bool app_spi_bus_timing_begin(
    app_spi_bus_timing_log_t *log,
    int64_t now_us,
    int64_t mutex_wait_us,
    int64_t quiesce_us,
    bool failed
)
{
    if (log == NULL) {
        return true;
    }

    log->operations = log->operations == UINT32_MAX ? UINT32_MAX : log->operations + 1U;
    if (failed) {
        log->failures = log->failures == UINT32_MAX ? UINT32_MAX : log->failures + 1U;
    }
    log->max_mutex_wait_us = app_spi_bus_timing_max(log->max_mutex_wait_us, mutex_wait_us);
    log->max_quiesce_us = app_spi_bus_timing_max(log->max_quiesce_us, quiesce_us);

    /* A failed begin has no end, so it never becomes the sampled operation. */
    log->sampled_operation =
        !failed &&
        (!log->started ||
         now_us - log->last_sample_us >= APP_SPI_BUS_TIMING_SUMMARY_PERIOD_US);

    return failed ||
        log->sampled_operation ||
        mutex_wait_us >= APP_SPI_BUS_TIMING_NOTABLE_WAIT_US ||
        quiesce_us >= APP_SPI_BUS_TIMING_NOTABLE_WAIT_US;
}


/*
 * Call once per operation at end. Returns true when the end line is to be
 * printed. *summary_due is set when this end also closes a summary window, in
 * which case *summary holds the values to print.
 */
static inline bool app_spi_bus_timing_end(
    app_spi_bus_timing_log_t *log,
    int64_t now_us,
    int64_t resume_us,
    int64_t elapsed_us,
    bool *summary_due,
    app_spi_bus_timing_summary_t *summary
)
{
    if (summary_due != NULL) {
        *summary_due = false;
    }
    if (log == NULL) {
        return true;
    }

    log->max_resume_us = app_spi_bus_timing_max(log->max_resume_us, resume_us);
    log->max_elapsed_us = app_spi_bus_timing_max(log->max_elapsed_us, elapsed_us);

    const bool sampled = log->sampled_operation;
    log->sampled_operation = false;
    if (sampled) {
        if (summary_due != NULL && summary != NULL) {
            *summary_due = true;
            summary->operations = log->operations;
            summary->failures = log->failures;
            summary->window_us = log->started ? now_us - log->last_sample_us : 0;
            summary->max_mutex_wait_us = log->max_mutex_wait_us;
            summary->max_quiesce_us = log->max_quiesce_us;
            summary->max_resume_us = log->max_resume_us;
            summary->max_elapsed_us = log->max_elapsed_us;
        }
        log->started = true;
        log->last_sample_us = now_us;
        log->operations = 0U;
        log->failures = 0U;
        log->max_mutex_wait_us = 0;
        log->max_quiesce_us = 0;
        log->max_resume_us = 0;
        log->max_elapsed_us = 0;
    }

    return sampled ||
        resume_us >= APP_SPI_BUS_TIMING_NOTABLE_WAIT_US ||
        elapsed_us >= APP_SPI_BUS_TIMING_NOTABLE_ELAPSED_US;
}
