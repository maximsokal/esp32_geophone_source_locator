#include <assert.h>
#include <string.h>

#include "nrf_cs_pair_adapter.h"
#include "nrf_cs_scheduler.h"
#include "nrf_cs_trace.h"

static nrf_cs_trace_entry_t entry(uint8_t kind, uint32_t attempt)
{
    nrf_cs_trace_entry_t e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.attempt_id = attempt;
    e.t_ms = 68402U;
    e.elapsed_ms = NRF_CS_TRACE_NO_ELAPSED;
    return e;
}

static void test_the_ring_keeps_order_and_counts_what_it_drops(void)
{
    nrf_cs_trace_t trace;
    nrf_cs_trace_init(&trace);
    nrf_cs_trace_entry_t out;
    assert(!nrf_cs_trace_pop(&trace, &out));
    for (uint32_t i = 0U; i < NRF_CS_TRACE_CAPACITY; ++i) {
        nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_EVENT, i + 1U);
        assert(nrf_cs_trace_push(&trace, &e));
    }
    nrf_cs_trace_entry_t extra = entry(NRF_CS_TRACE_EVENT, 999U);
    assert(!nrf_cs_trace_push(&trace, &extra));                 /* full: the new one goes, the queued ones stay */
    assert(!nrf_cs_trace_push(&trace, &extra));
    assert(nrf_cs_trace_take_dropped(&trace) == 2U);
    assert(nrf_cs_trace_take_dropped(&trace) == 0U);            /* reported once */
    for (uint32_t i = 0U; i < NRF_CS_TRACE_CAPACITY; ++i) {
        assert(nrf_cs_trace_pop(&trace, &out));
        assert(out.attempt_id == i + 1U);                       /* first in, first out */
    }
    assert(!nrf_cs_trace_pop(&trace, &out));
    /* The counters wrap far past the capacity without breaking the order. */
    for (uint32_t round = 0U; round < 5U * NRF_CS_TRACE_CAPACITY; ++round) {
        nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_EVENT, round);
        assert(nrf_cs_trace_push(&trace, &e));
        assert(nrf_cs_trace_pop(&trace, &out) && out.attempt_id == round);
    }
    assert(!nrf_cs_trace_push(NULL, &extra) && !nrf_cs_trace_push(&trace, NULL));
    assert(!nrf_cs_trace_pop(NULL, &out) && !nrf_cs_trace_pop(&trace, NULL));
    assert(nrf_cs_trace_take_dropped(NULL) == 0U);
}

static void test_names_cover_every_phase_and_outcome(void)
{
    assert(strcmp(nrf_cs_trace_phase_name(NRF_CS_PAIR_PHASE_CONNECTING), "CONNECTING") == 0);
    assert(strcmp(nrf_cs_trace_phase_name(NRF_CS_PAIR_PHASE_SECURING), "SECURING") == 0);
    assert(strcmp(nrf_cs_trace_phase_name(NRF_CS_PAIR_PHASE_PROCEDURE), "PROCEDURE") == 0);
    assert(strcmp(nrf_cs_trace_phase_name(NRF_CS_PAIR_PHASE_CLEANUP), "CLEANUP") == 0);
    assert(strcmp(nrf_cs_trace_phase_name(200U), "?") == 0);
    for (uint8_t phase = NRF_CS_PAIR_PHASE_IDLE; phase <= NRF_CS_PAIR_PHASE_CLEANUP; ++phase) {
        assert(strcmp(nrf_cs_trace_phase_name(phase), "?") != 0);
    }
    assert(strcmp(nrf_cs_trace_outcome_name(NRF_CS_OUTCOME_SUCCESS), "SUCCESS") == 0);
    assert(strcmp(nrf_cs_trace_outcome_name(NRF_CS_OUTCOME_TIMEOUT), "TIMEOUT") == 0);
    assert(strcmp(nrf_cs_trace_outcome_name(NRF_CS_OUTCOME_TRANSPORT_ERROR), "TRANSPORT_ERROR") == 0);
    assert(strcmp(nrf_cs_trace_outcome_name(NRF_CS_OUTCOME_OUTLIER), "OUTLIER") == 0);
    assert(strcmp(nrf_cs_trace_outcome_name(77U), "?") == 0);
}

/* The lines of the bench log 2026-11-02 10:36 (node 4 measuring MAIN, attempt 1, pair 5) as the console shows them. */
static void test_lines_carry_the_stage_and_the_time_since_the_attempt_started(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_EXECUTE, 1U);
    e.a = 500;
    e.b = (int32_t)((4U << 24U) | (0U << 16U) | (1U << 8U) | 5U);
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 execute init=4 refl=0 try=1 pair=5 budget=500ms") == 0);

    e = entry(NRF_CS_TRACE_EVENT, 1U);
    e.label = "SECURITY_READY";
    e.phase = NRF_CS_PAIR_PHASE_SECURING;
    e.elapsed_ms = 312U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 ev=SECURITY_READY status=0 phase=SECURING +312ms") == 0);
    e.label = "TRANSPORT_ERROR";
    e.a = 9;
    e.phase = NRF_CS_PAIR_PHASE_CLEANUP;
    e.elapsed_ms = 556U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 ev=TRANSPORT_ERROR status=9 phase=CLEANUP +556ms") == 0);
    e.elapsed_ms = NRF_CS_TRACE_NO_ELAPSED;                      /* the reflector side has no attempt start */
    e.attempt_id = 0U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=0 ev=TRANSPORT_ERROR status=9 phase=CLEANUP") == 0);

    e = entry(NRF_CS_TRACE_TIMEOUT, 1U);
    e.a = 500;
    e.phase = NRF_CS_PAIR_PHASE_SECURING;
    e.elapsed_ms = 501U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 TIMEOUT phase=SECURING budget=500ms +501ms") == 0);

    e = entry(NRF_CS_TRACE_RESULT, 1U);
    e.label = "SUCCESS";
    e.a = 3420;
    e.b = 870;
    e.phase = NRF_CS_PAIR_PHASE_PROCEDURE;
    e.elapsed_ms = 940U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 result=SUCCESS dist_mm=3420 conf=870 phase=PROCEDURE +940ms") == 0);

    e = entry(NRF_CS_TRACE_RF, 1U);
    e.a = (int32_t)0xf804b0c4U;
    e.b = 0x4023c032;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 rf signal=0xf804b0c4 quality=0x4023c032") == 0);

    e = entry(NRF_CS_TRACE_ESTIMATES, 1U);                       /* all three methods of the library, and "best" */
    e.a = 1760;
    e.b = 220;
    e.c = NRF_CS_TRACE_UNAVAILABLE;
    e.d = 1760;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 est ifft_mm=1760 phase_mm=220 rtt_mm=n/a best_mm=1760") == 0);

    assert(nrf_cs_trace_format_dropped(7U, line, sizeof(line)) > 0);
    assert(strstr(line, "trace_dropped=7") != NULL);
}

static void test_the_longest_line_fits_and_bad_arguments_are_refused(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_EVENT, UINT32_MAX);
    e.label = "INCOMING_DISCONNECTED";
    e.a = -2147483647;
    e.t_ms = UINT32_MAX;
    e.phase = NRF_CS_PAIR_PHASE_CAPABILITIES;
    e.elapsed_ms = UINT32_MAX - 1U;
    const int length = nrf_cs_trace_format(&e, line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line));        /* nothing is cut at the worst case */

    char small[16];
    e.elapsed_ms = 5U;
    const int wanted = nrf_cs_trace_format(&e, small, sizeof(small));
    assert(wanted > (int)sizeof(small) && small[sizeof(small) - 1U] == '\0');   /* cut, but terminated */
    assert(nrf_cs_trace_format(NULL, line, sizeof(line)) < 0);
    assert(nrf_cs_trace_format(&e, NULL, sizeof(line)) < 0);
    assert(nrf_cs_trace_format(&e, line, 0U) < 0);
    e.kind = 0U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) < 0);
    e.kind = 99U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) < 0);
    assert(nrf_cs_trace_format_dropped(1U, NULL, 8U) < 0);
}

int main(void)
{
    test_the_ring_keeps_order_and_counts_what_it_drops();
    test_names_cover_every_phase_and_outcome();
    test_lines_carry_the_stage_and_the_time_since_the_attempt_started();
    test_the_longest_line_fits_and_bad_arguments_are_refused();
    return 0;
}
