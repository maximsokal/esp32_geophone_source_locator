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
    assert(strcmp(line, "cs: t=68402 a=1 est ifft_mm=1760 phase_mm=220 rtt_mm=n/a best_mm=1760 method=ifft") == 0);

    assert(nrf_cs_trace_format_dropped(7U, line, sizeof(line)) > 0);
    assert(strstr(line, "trace_dropped=7") != NULL);
}

/* CS-ESTIMATOR-01: every "est" line says which estimate "best" was taken from. The rows are attempts of the bench logs
 * 2026-11-02 (MAIN 20:12 and REMOTE 20:22): the three methods disagree by metres and the line shows which one was used. */
static void test_the_est_line_names_the_method_that_made_the_distance(void)
{
    static const struct {
        int32_t ifft, phase, rtt, best;
        const char *line;
    } rows[] = {
        { 6272, 8917, 9550, 6272, "cs: t=68402 a=1 est ifft_mm=6272 phase_mm=8917 rtt_mm=9550 best_mm=6272 method=ifft" },
        { 3806, 9406, 3135, 3806, "cs: t=68402 a=1 est ifft_mm=3806 phase_mm=9406 rtt_mm=3135 best_mm=3806 method=ifft" },
        { 3556, 3756, 933, 3556, "cs: t=68402 a=1 est ifft_mm=3556 phase_mm=3756 rtt_mm=933 best_mm=3556 method=ifft" },
        { NRF_CS_TRACE_UNAVAILABLE, 4638, 3164, 4638,
          "cs: t=68402 a=1 est ifft_mm=n/a phase_mm=4638 rtt_mm=3164 best_mm=4638 method=phase" },
        { NRF_CS_TRACE_UNAVAILABLE, NRF_CS_TRACE_UNAVAILABLE, 933, 933,
          "cs: t=68402 a=1 est ifft_mm=n/a phase_mm=n/a rtt_mm=933 best_mm=933 method=rtt" },
        { NRF_CS_TRACE_UNAVAILABLE, NRF_CS_TRACE_UNAVAILABLE, NRF_CS_TRACE_UNAVAILABLE, NRF_CS_TRACE_UNAVAILABLE,
          "cs: t=68402 a=1 est ifft_mm=n/a phase_mm=n/a rtt_mm=n/a best_mm=n/a method=none" },
        /* "best" that is none of the three (a library that changed its rule) is never given a wrong name. */
        { 3806, 9406, 3135, 5000, "cs: t=68402 a=1 est ifft_mm=3806 phase_mm=9406 rtt_mm=3135 best_mm=5000 method=none" },
    };
    char line[NRF_CS_TRACE_LINE_CAPACITY];

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_ESTIMATES, 1U);
        e.a = rows[i].ifft;
        e.b = rows[i].phase;
        e.c = rows[i].rtt;
        e.d = rows[i].best;
        assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
        assert(strcmp(line, rows[i].line) == 0);
    }
}

/* CS-IFFT-GAP-01: an est line that knows the frequency set says how complete it was. */
static void test_the_est_line_reports_how_complete_the_frequency_set_was(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_ESTIMATES, 1U);

    e.a = 7322;
    e.b = 9378;
    e.c = 10106;
    e.d = 7322;
    e.tones_planned = 75U;
    e.tones_usable = 63U;
    e.tones_gap = 5U;
    e.tones_lead = 5U;
    e.tones_tail = 0U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 est ifft_mm=7322 phase_mm=9378 rtt_mm=10106 best_mm=7322 method=ifft "
                        "tones=63/75 gap=5 lead=5 tail=0") == 0);
    e.tones_planned = 0U;                                           /* not known: the line is the old one */
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, "tones=") == NULL);
    e.tones_planned = 255U;
    e.tones_usable = 255U;
    e.tones_gap = 255U;
    e.tones_lead = 255U;
    e.tones_tail = 255U;
    e.t_ms = UINT32_MAX;
    e.attempt_id = UINT32_MAX;
    e.a = e.b = e.c = e.d = -2147483647;
    const int length = nrf_cs_trace_format(&e, line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line));
}

/* How sharp the IFFT peak was is logged with every estimate that knows it, in tenths, and is never invented. */
static void test_the_est_line_reports_how_sharp_the_ifft_peak_was(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_ESTIMATES, 1U);

    e.a = 7322; e.b = 9378; e.c = 10106; e.d = 7322;
    e.tones_planned = 75U; e.tones_usable = 63U; e.tones_gap = 5U; e.tones_lead = 5U; e.tones_tail = 0U;
    e.peak_floor_x10 = 123U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=1 est ifft_mm=7322 phase_mm=9378 rtt_mm=10106 best_mm=7322 method=ifft "
                        "tones=63/75 gap=5 lead=5 tail=0 pf=12.3") == 0);
    e.peak_floor_x10 = 9U;                                          /* below 1.0: still printed, as 0.9 */
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, " pf=0.9") != NULL);
    e.tones_planned = 0U;                                           /* the tone fields are independent of it */
    e.peak_floor_x10 = 300U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, "tones=") == NULL && strstr(line, " pf=30.0") != NULL);
    e.peak_floor_x10 = 0U;                                          /* not known: the line is the old one */
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, "pf=") == NULL);
    e.tones_planned = 255U; e.tones_usable = 255U; e.tones_gap = 255U; e.tones_lead = 255U; e.tones_tail = 255U;
    e.peak_floor_x10 = UINT16_MAX;
    e.t_ms = UINT32_MAX; e.attempt_id = UINT32_MAX;
    e.a = e.b = e.c = e.d = -2147483647;
    const int length = nrf_cs_trace_format(&e, line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line) && strstr(line, " pf=6553.5") != NULL);
}

/* CS-SECURITY-01: status 9 of a TRANSPORT_ERROR in SECURING is Zephyr's BT_SECURITY_ERR_UNSPECIFIED (pairing failed, no
 * reason given); the line says so, and only for that event in that phase. */
static void test_a_security_failure_is_named_in_the_event_line(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_EVENT, 3U);

    e.label = "TRANSPORT_ERROR";
    e.a = 9;
    e.phase = NRF_CS_PAIR_PHASE_SECURING;
    e.elapsed_ms = 233U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=3 ev=TRANSPORT_ERROR status=9 phase=SECURING err=SECURITY_UNSPECIFIED +233ms") == 0);

    static const char *const names[] = {
        "SECURITY_SUCCESS", "SECURITY_AUTH_FAIL", "SECURITY_PIN_OR_KEY_MISSING", "SECURITY_OOB_NOT_AVAILABLE",
        "SECURITY_AUTH_REQUIREMENT", "SECURITY_PAIR_NOT_SUPPORTED", "SECURITY_PAIR_NOT_ALLOWED",
        "SECURITY_INVALID_PARAM", "SECURITY_KEY_REJECTED", "SECURITY_UNSPECIFIED",
    };
    for (int status = 0; status < 10; ++status) {
        const char *reason = nrf_cs_trace_status_reason("TRANSPORT_ERROR", NRF_CS_PAIR_PHASE_SECURING, status);
        assert(reason != NULL && strcmp(reason, names[status]) == 0);
    }
    assert(nrf_cs_trace_status_reason("TRANSPORT_ERROR", NRF_CS_PAIR_PHASE_SECURING, 10) == NULL);
    assert(nrf_cs_trace_status_reason("TRANSPORT_ERROR", NRF_CS_PAIR_PHASE_SECURING, -1) == NULL);
    /* The same number in another phase is an HCI status, not a security error: no name is invented for it. */
    assert(nrf_cs_trace_status_reason("TRANSPORT_ERROR", NRF_CS_PAIR_PHASE_CAPABILITIES, 9) == NULL);
    assert(nrf_cs_trace_status_reason("TRANSPORT_ERROR", NRF_CS_PAIR_PHASE_CLEANUP, 9) == NULL);
    assert(nrf_cs_trace_status_reason("DISCONNECTED", NRF_CS_PAIR_PHASE_SECURING, 9) == NULL);
    assert(nrf_cs_trace_status_reason(NULL, NRF_CS_PAIR_PHASE_SECURING, 9) == NULL);
    e.label = "SECURITY_READY";
    e.a = 0;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, "err=") == NULL);
}

/* CS-TIME-01: a result that carries the timing says when the distance was ready and how long the cleanup took, and flags
 * an attempt that ran past its budget (MAIN attempt 2 of the 20:12 log). */
static void test_the_result_line_separates_the_measurement_from_the_cleanup(void)
{
    char line[NRF_CS_TRACE_LINE_CAPACITY];
    nrf_cs_trace_entry_t e = entry(NRF_CS_TRACE_RESULT, 2U);

    e.label = "SUCCESS";
    e.a = 6194;
    e.b = 755;
    e.phase = NRF_CS_PAIR_PHASE_IDLE;
    e.elapsed_ms = 5262U;
    e.c = 1268;
    e.d = 3994;
    e.flags = NRF_CS_TRACE_FLAG_TIMING | NRF_CS_TRACE_FLAG_OVER_BUDGET | NRF_CS_TRACE_FLAG_TIMEOUT_IN_CLEANUP;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=2 result=SUCCESS dist_mm=6194 conf=755 phase=IDLE measured=1268ms cleanup=3994ms "
                        "over_budget=1 timeout_in_cleanup=1 +5262ms") == 0);

    /* A prompt cleanup: timing, no flags. */
    e.c = 1428;
    e.d = 53;
    e.elapsed_ms = 1481U;
    e.flags = NRF_CS_TRACE_FLAG_TIMING;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=2 result=SUCCESS dist_mm=6194 conf=755 phase=IDLE measured=1428ms cleanup=53ms "
                        "+1481ms") == 0);

    /* Only the over-budget flag, and only the cleanup-timer flag. */
    e.flags = NRF_CS_TRACE_FLAG_TIMING | NRF_CS_TRACE_FLAG_OVER_BUDGET;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, " over_budget=1") != NULL && strstr(line, "timeout_in_cleanup") == NULL);
    e.flags = NRF_CS_TRACE_FLAG_TIMING | NRF_CS_TRACE_FLAG_TIMEOUT_IN_CLEANUP;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strstr(line, " timeout_in_cleanup=1") != NULL && strstr(line, "over_budget") == NULL);

    /* Without the timing flag the line is the old one (a node whose adapter has no clock). */
    e.flags = 0U;
    assert(nrf_cs_trace_format(&e, line, sizeof(line)) > 0);
    assert(strcmp(line, "cs: t=68402 a=2 result=SUCCESS dist_mm=6194 conf=755 phase=IDLE +1481ms") == 0);

    /* The longest result line still fits the buffer. */
    e.t_ms = UINT32_MAX;
    e.attempt_id = UINT32_MAX;
    e.a = -2147483647;
    e.b = 1000;
    e.c = INT32_MAX;
    e.d = INT32_MAX;
    e.label = "TRANSPORT_ERROR";
    e.elapsed_ms = UINT32_MAX - 1U;
    e.flags = NRF_CS_TRACE_FLAG_TIMING | NRF_CS_TRACE_FLAG_OVER_BUDGET | NRF_CS_TRACE_FLAG_TIMEOUT_IN_CLEANUP;
    const int length = nrf_cs_trace_format(&e, line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line));
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
    test_the_est_line_names_the_method_that_made_the_distance();
    test_the_est_line_reports_how_complete_the_frequency_set_was();
    test_the_est_line_reports_how_sharp_the_ifft_peak_was();
    test_a_security_failure_is_named_in_the_event_line();
    test_the_result_line_separates_the_measurement_from_the_cleanup();
    test_the_longest_line_fits_and_bad_arguments_are_refused();
    return 0;
}
