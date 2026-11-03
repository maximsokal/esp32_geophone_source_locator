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

const char *nrf_cs_trace_status_reason(const char *event_label, uint8_t phase, int status)
{
    static const char *const security[] = {
        "SECURITY_SUCCESS", "SECURITY_AUTH_FAIL", "SECURITY_PIN_OR_KEY_MISSING", "SECURITY_OOB_NOT_AVAILABLE",
        "SECURITY_AUTH_REQUIREMENT", "SECURITY_PAIR_NOT_SUPPORTED", "SECURITY_PAIR_NOT_ALLOWED",
        "SECURITY_INVALID_PARAM", "SECURITY_KEY_REJECTED", "SECURITY_UNSPECIFIED",
    };
    if (event_label == NULL || strcmp(event_label, "TRANSPORT_ERROR") != 0 || phase != NRF_CS_PAIR_PHASE_SECURING ||
        status < 0 || (size_t)status >= sizeof(security) / sizeof(security[0])) {
        return NULL;
    }
    return security[status];
}

/* " +123ms" when the attempt start is known, nothing otherwise. */
static int append_elapsed(char *line, size_t capacity, int used, uint32_t elapsed_ms)
{
    if (used < 0 || (size_t)used >= capacity || elapsed_ms == NRF_CS_TRACE_NO_ELAPSED) return used;
    const int written = snprintf(line + used, capacity - (size_t)used, " +%ums", (unsigned)elapsed_ms);
    return written < 0 ? -1 : used + written;
}

/* A distance in mm, or "n/a" for an estimate the library did not make. */
static const char *distance_text(int32_t mm, char buffer[16])
{
    if (mm == NRF_CS_TRACE_UNAVAILABLE) return "n/a";
    (void)snprintf(buffer, 16U, "%d", (int)mm);
    return buffer;
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
        case NRF_CS_TRACE_EVENT: {
            const char *reason = nrf_cs_trace_status_reason(label, entry->phase, (int)entry->a);
            used = snprintf(line, capacity, "cs: t=%u a=%u ev=%s status=%d phase=%s%s%s",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, label, (int)entry->a,
                nrf_cs_trace_phase_name(entry->phase), reason != NULL ? " err=" : "", reason != NULL ? reason : "");
            break;
        }
        case NRF_CS_TRACE_TIMEOUT:
            used = snprintf(line, capacity, "cs: t=%u a=%u TIMEOUT phase=%s budget=%dms",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, nrf_cs_trace_phase_name(entry->phase),
                (int)entry->a);
            break;
        case NRF_CS_TRACE_RESULT:
            used = snprintf(line, capacity, "cs: t=%u a=%u result=%s dist_mm=%d conf=%d phase=%s",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, label, (int)entry->a, (int)entry->b,
                nrf_cs_trace_phase_name(entry->phase));
            if (used >= 0 && (size_t)used < capacity && (entry->flags & NRF_CS_TRACE_FLAG_TIMING) != 0U) {
                /* The distance was ready at "measured"; "cleanup" is the disconnect after it. */
                const int more = snprintf(line + used, capacity - (size_t)used, " measured=%ums cleanup=%ums%s%s",
                    (unsigned)entry->c, (unsigned)entry->d,
                    (entry->flags & NRF_CS_TRACE_FLAG_OVER_BUDGET) != 0U ? " over_budget=1" : "",
                    (entry->flags & NRF_CS_TRACE_FLAG_TIMEOUT_IN_CLEANUP) != 0U ? " timeout_in_cleanup=1" : "");
                used = more < 0 ? -1 : used + more;
            }
            break;
        case NRF_CS_TRACE_RF:
            return snprintf(line, capacity, "cs: t=%u a=%u rf signal=0x%08x quality=0x%08x",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, (unsigned)entry->a, (unsigned)entry->b);
        case NRF_CS_TRACE_ESTIMATES: {
            char ifft[16], phase[16], rtt[16], best[16];
            char tones[48] = "";
            char sharp[16] = "";
            char stock[24] = "";
            char why[32] = "";
            if (entry->peak_floor_x10 > 0U) {
                /* How sharp the IFFT peak was: logged only, nothing is rejected on it (nrf_cs_ifft_quality.h). */
                (void)snprintf(sharp, sizeof(sharp), " pf=%u.%u", (unsigned)(entry->peak_floor_x10 / 10U),
                    (unsigned)(entry->peak_floor_x10 % 10U));
            }
            if ((entry->flags & NRF_CS_TRACE_FLAG_STOCK_IFFT) != 0U && entry->e != NRF_CS_TRACE_UNAVAILABLE && entry->e != entry->a) {
                /* The library's own IFFT, when the gap fill changed it: both are logged, the one in ifft_mm is the one used. */
                (void)snprintf(stock, sizeof(stock), " stock_mm=%d", (int)entry->e);
            }
            if (entry->why != NULL) {
                (void)snprintf(why, sizeof(why), " why=%s", entry->why);
            }
            if (entry->tones_planned > 0U) {
                /* How complete the frequency set was: the IFFT sees every missing channel as a zero (CS-IFFT-GAP-01). */
                (void)snprintf(tones, sizeof(tones), " tones=%u/%u gap=%u lead=%u tail=%u", (unsigned)entry->tones_usable,
                    (unsigned)entry->tones_planned, (unsigned)entry->tones_gap, (unsigned)entry->tones_lead,
                    (unsigned)entry->tones_tail);
            }
            /* "method" names the estimate that "best" was taken from (CS-ESTIMATOR-01). */
            return snprintf(line, capacity, "cs: t=%u a=%u est ifft_mm=%s phase_mm=%s rtt_mm=%s best_mm=%s method=%s%s%s%s%s",
                (unsigned)entry->t_ms, (unsigned)entry->attempt_id, distance_text(entry->a, ifft),
                distance_text(entry->b, phase), distance_text(entry->c, rtt), distance_text(entry->d, best),
                nrf_cs_distance_method_name(nrf_cs_distance_method_of(entry->a, entry->b, entry->c, entry->d)), tones, sharp, stock, why);
        }
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
