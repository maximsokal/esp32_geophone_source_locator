#include "nrf_cs_trace_zephyr.h"

#include "nrf_cs_trace.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* A line is ~100 characters, about 9 ms of UART at 115200: a few per 50 ms main-loop pass keep up with a cycle. */
#define NRF_CS_TRACE_LINES_PER_DRAIN 4U

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG

static nrf_cs_trace_t s_trace;
static struct k_spinlock s_trace_lock;
static bool s_trace_ready;
static uint32_t s_attempt_id;
static uint32_t s_attempt_start_ms;

static void push_locked(nrf_cs_trace_entry_t *entry)
{
    k_spinlock_key_t key = k_spin_lock(&s_trace_lock);
    if (!s_trace_ready) {
        nrf_cs_trace_init(&s_trace);
        s_trace_ready = true;
    }
    entry->t_ms = k_uptime_get_32();
    /* Events that belong to the attempt in progress carry the time since it was accepted. */
    entry->elapsed_ms = entry->attempt_id != 0U && entry->attempt_id == s_attempt_id
        ? entry->t_ms - s_attempt_start_ms : NRF_CS_TRACE_NO_ELAPSED;
    (void)nrf_cs_trace_push(&s_trace, entry);
    k_spin_unlock(&s_trace_lock, key);
}

void nrf_cs_trace_zephyr_execute(uint32_t attempt_id, uint8_t initiator, uint8_t reflector,
    uint8_t attempt_number, uint8_t pair_index, uint32_t budget_ms)
{
    s_attempt_id = attempt_id;
    s_attempt_start_ms = k_uptime_get_32();
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_EXECUTE, .attempt_id = attempt_id, .a = (int32_t)budget_ms,
        .b = (int32_t)(((uint32_t)initiator << 24U) | ((uint32_t)reflector << 16U) |
                       ((uint32_t)attempt_number << 8U) | pair_index),
    };
    push_locked(&entry);
}

void nrf_cs_trace_zephyr_event(const char *label, uint32_t attempt_id, int status, uint8_t phase)
{
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_EVENT, .attempt_id = attempt_id, .label = label, .a = status, .phase = phase,
    };
    push_locked(&entry);
}

void nrf_cs_trace_zephyr_timeout(uint32_t attempt_id, uint8_t phase, uint32_t budget_ms)
{
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_TIMEOUT, .attempt_id = attempt_id, .a = (int32_t)budget_ms, .phase = phase,
    };
    push_locked(&entry);
}

void nrf_cs_trace_zephyr_result(uint32_t attempt_id, uint8_t outcome, int32_t distance_mm,
    uint16_t confidence, uint8_t phase)
{
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_RESULT, .attempt_id = attempt_id, .label = nrf_cs_trace_outcome_name(outcome),
        .a = distance_mm, .b = (int32_t)confidence, .phase = phase,
    };
    push_locked(&entry);
}

void nrf_cs_trace_zephyr_rf(uint32_t attempt_id, uint32_t signal_word, uint32_t quality_word)
{
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_RF, .attempt_id = attempt_id, .a = (int32_t)signal_word, .b = (int32_t)quality_word,
    };
    push_locked(&entry);
}

static int32_t metres_to_mm(float metres)
{
    if (!isfinite(metres) || metres < 0.0f || metres > 2000000.0f) return NRF_CS_TRACE_UNAVAILABLE;
    return (int32_t)(metres * 1000.0f + 0.5f);
}

void nrf_cs_trace_zephyr_estimates(uint32_t attempt_id, float ifft_m, float phase_slope_m, float rtt_m, float best_m)
{
    nrf_cs_trace_entry_t entry = {
        .kind = NRF_CS_TRACE_ESTIMATES, .attempt_id = attempt_id, .a = metres_to_mm(ifft_m),
        .b = metres_to_mm(phase_slope_m), .c = metres_to_mm(rtt_m), .d = metres_to_mm(best_m),
    };
    push_locked(&entry);
}

void nrf_cs_trace_zephyr_console_drain(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];

    for (uint32_t printed = 0U; printed < NRF_CS_TRACE_LINES_PER_DRAIN; ++printed) {
        nrf_cs_trace_entry_t entry;
        bool have = false;
        uint32_t dropped = 0U;

        k_spinlock_key_t key = k_spin_lock(&s_trace_lock);
        if (s_trace_ready) {
            dropped = nrf_cs_trace_take_dropped(&s_trace);
            have = dropped == 0U && nrf_cs_trace_pop(&s_trace, &entry);
        }
        k_spin_unlock(&s_trace_lock, key);

        if (dropped != 0U) {
            if (nrf_cs_trace_format_dropped(dropped, line, sizeof(line)) >= 0) printk("%s\n", line);
            continue;
        }
        if (!have) return;
        if (nrf_cs_trace_format(&entry, line, sizeof(line)) >= 0) printk("%s\n", line);
    }
}

#else

void nrf_cs_trace_zephyr_execute(uint32_t attempt_id, uint8_t initiator, uint8_t reflector,
    uint8_t attempt_number, uint8_t pair_index, uint32_t budget_ms)
{
    (void)attempt_id; (void)initiator; (void)reflector; (void)attempt_number; (void)pair_index; (void)budget_ms;
}

void nrf_cs_trace_zephyr_event(const char *label, uint32_t attempt_id, int status, uint8_t phase)
{
    (void)label; (void)attempt_id; (void)status; (void)phase;
}

void nrf_cs_trace_zephyr_timeout(uint32_t attempt_id, uint8_t phase, uint32_t budget_ms)
{
    (void)attempt_id; (void)phase; (void)budget_ms;
}

void nrf_cs_trace_zephyr_result(uint32_t attempt_id, uint8_t outcome, int32_t distance_mm,
    uint16_t confidence, uint8_t phase)
{
    (void)attempt_id; (void)outcome; (void)distance_mm; (void)confidence; (void)phase;
}

void nrf_cs_trace_zephyr_rf(uint32_t attempt_id, uint32_t signal_word, uint32_t quality_word)
{
    (void)attempt_id; (void)signal_word; (void)quality_word;
}

void nrf_cs_trace_zephyr_estimates(uint32_t attempt_id, float ifft_m, float phase_slope_m, float rtt_m, float best_m)
{
    (void)attempt_id; (void)ifft_m; (void)phase_slope_m; (void)rtt_m; (void)best_m;
}

void nrf_cs_trace_zephyr_console_drain(void) {}

#endif
