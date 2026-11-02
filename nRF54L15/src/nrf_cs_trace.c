#include "nrf_cs_trace.h"

#include <stdio.h>
#include <string.h>

#include "nrf_cs_pair_adapter.h"
#include "nrf_cs_scheduler.h"

void nrf_cs_trace_init(nrf_cs_trace_t *trace)
{
    if (trace != NULL) memset(trace, 0, sizeof(*trace));
}

bool nrf_cs_trace_push(nrf_cs_trace_t *trace, const nrf_cs_trace_entry_t *entry)
{
    if (trace == NULL || entry == NULL) return false;
    if (trace->head - trace->tail >= NRF_CS_TRACE_CAPACITY) {
        if (trace->dropped < UINT32_MAX) ++trace->dropped;
        return false;
    }
    trace->entries[trace->head % NRF_CS_TRACE_CAPACITY] = *entry;
    ++trace->head;
    return true;
}

bool nrf_cs_trace_pop(nrf_cs_trace_t *trace, nrf_cs_trace_entry_t *entry)
{
    if (trace == NULL || entry == NULL || trace->head == trace->tail) return false;
    *entry = trace->entries[trace->tail % NRF_CS_TRACE_CAPACITY];
    ++trace->tail;
    return true;
}

uint32_t nrf_cs_trace_take_dropped(nrf_cs_trace_t *trace)
{
    if (trace == NULL) return 0U;
    const uint32_t dropped = trace->dropped;
    trace->dropped = 0U;
    return dropped;
}

const char *nrf_cs_trace_phase_name(uint8_t phase)
{
    switch (phase) {
        case NRF_CS_PAIR_PHASE_IDLE: return "IDLE";
        case NRF_CS_PAIR_PHASE_CONNECTING: return "CONNECTING";
        case NRF_CS_PAIR_PHASE_SECURING: return "SECURING";
        case NRF_CS_PAIR_PHASE_MTU: return "MTU";
        case NRF_CS_PAIR_PHASE_DISCOVERING: return "DISCOVERING";
        case NRF_CS_PAIR_PHASE_FEATURES: return "FEATURES";
        case NRF_CS_PAIR_PHASE_CAPABILITIES: return "CAPABILITIES";
        case NRF_CS_PAIR_PHASE_CONFIG: return "CONFIG";
        case NRF_CS_PAIR_PHASE_CS_SECURITY: return "CS_SECURITY";
        case NRF_CS_PAIR_PHASE_PROCEDURE: return "PROCEDURE";
        case NRF_CS_PAIR_PHASE_CLEANUP: return "CLEANUP";
        default: return "?";
    }
}

const char *nrf_cs_trace_outcome_name(uint8_t outcome)
{
    switch (outcome) {
        case NRF_CS_OUTCOME_SUCCESS: return "SUCCESS";
        case NRF_CS_OUTCOME_TIMEOUT: return "TIMEOUT";
        case NRF_CS_OUTCOME_TRANSPORT_ERROR: return "TRANSPORT_ERROR";
        case NRF_CS_OUTCOME_OUTLIER: return "OUTLIER";
        default: return "?";
    }
}

/* " +123ms" when the attempt start is known, nothing otherwise. */
static int append_elapsed(char *line, size_t capacity, int used, uint32_t elapsed_ms)
{
    if (used < 0 || (size_t)used >= capacity || elapsed_ms == NRF_CS_TRACE_NO_ELAPSED) return used;
    const int written = snprintf(line + used, capacity - (size_t)used, " +%ums", (unsigned)elapsed_ms);
    return written < 0 ? -1 : used + written;
}

int nrf_cs_trace_format(const nrf_cs_trace_entry_t *entry, char *line, size_t capacity)
{
    if (entry == NULL || line == NULL || capacity == 0U) return -1;
    const char *label = entry->label != NULL ? entry->label : "?";
    int used;

    switch (entry->kind) {
        case NRF_CS_TRACE_EXECUTE:
            /* b = initiator:8 reflector:8 try:8 pair:8 (most significant first), a = budget in ms */
            used = snprintf(line, capacity,
                "cs: t=%u a=%u execute init=%u refl=%u try=%u pair=%u budget=%dms",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id,
                (unsigned)(((uint32_t)entry->b >> 24U) & 0xffU), (unsigned)(((uint32_t)entry->b >> 16U) & 0xffU),
                (unsigned)(((uint32_t)entry->b >> 8U) & 0xffU), (unsigned)((uint32_t)entry->b & 0xffU),
                (int)entry->a);
            return used;
        case NRF_CS_TRACE_EVENT:
            used = snprintf(line, capacity, "cs: t=%u a=%u ev=%s status=%d phase=%s",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, label, (int)entry->a,
                nrf_cs_trace_phase_name(entry->phase));
            break;
        case NRF_CS_TRACE_TIMEOUT:
            used = snprintf(line, capacity, "cs: t=%u a=%u TIMEOUT phase=%s budget=%dms",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, nrf_cs_trace_phase_name(entry->phase),
                (int)entry->a);
            break;
        case NRF_CS_TRACE_RESULT:
            used = snprintf(line, capacity, "cs: t=%u a=%u result=%s dist_mm=%d conf=%d phase=%s",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, label, (int)entry->a, (int)entry->b,
                nrf_cs_trace_phase_name(entry->phase));
            break;
        case NRF_CS_TRACE_RF:
            return snprintf(line, capacity, "cs: t=%u a=%u rf signal=0x%08x quality=0x%08x",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, (unsigned)entry->a, (unsigned)entry->b);
        default:
            return -1;
    }
    return append_elapsed(line, capacity, used, entry->elapsed_ms);
}

int nrf_cs_trace_format_dropped(uint32_t dropped, char *line, size_t capacity)
{
    if (line == NULL || capacity == 0U) return -1;
    return snprintf(line, capacity, "cs: trace_dropped=%u (the console fell behind the BLE events)", (unsigned)dropped);
}
