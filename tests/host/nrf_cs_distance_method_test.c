#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_cs_distance_method.h"

/* CS-ESTIMATOR-01. cs_de_calc() fills IFFT, phase slope and RTT and copies the first finite one into "best". The attempt
 * has to say which one, otherwise a wrong distance cannot be traced to the method that made it. */

static void test_metres_become_millimetres_or_unavailable(void)
{
    assert(nrf_cs_distance_mm_from_m(6.272f) == 6272);
    assert(nrf_cs_distance_mm_from_m(0.0f) == 0);
    assert(nrf_cs_distance_mm_from_m(0.0004f) == 0 && nrf_cs_distance_mm_from_m(0.0006f) == 1);   /* rounds to nearest */
    assert(nrf_cs_distance_mm_from_m(NAN) == NRF_CS_DISTANCE_UNAVAILABLE);
    assert(nrf_cs_distance_mm_from_m(INFINITY) == NRF_CS_DISTANCE_UNAVAILABLE);
    assert(nrf_cs_distance_mm_from_m(-0.001f) == NRF_CS_DISTANCE_UNAVAILABLE);
    assert(nrf_cs_distance_mm_from_m(2000001.0f) == NRF_CS_DISTANCE_UNAVAILABLE);
}

static void test_the_method_follows_the_rule_of_the_library(void)
{
    const int32_t none = NRF_CS_DISTANCE_UNAVAILABLE;

    /* Rows of the bench logs 2026-11-02: the estimates differ by metres, the first finite one is the distance. */
    assert(nrf_cs_distance_method_of(6272, 8917, 9550, 6272) == NRF_CS_DISTANCE_METHOD_IFFT);
    assert(nrf_cs_distance_method_of(3806, 9406, 3135, 3806) == NRF_CS_DISTANCE_METHOD_IFFT);
    assert(nrf_cs_distance_method_of(3556, 3756, 933, 3556) == NRF_CS_DISTANCE_METHOD_IFFT);
    /* No IFFT: the phase slope, and without it the RTT. */
    assert(nrf_cs_distance_method_of(none, 4638, 3164, 4638) == NRF_CS_DISTANCE_METHOD_PHASE);
    assert(nrf_cs_distance_method_of(none, none, 933, 933) == NRF_CS_DISTANCE_METHOD_RTT);
    /* Equal values of two methods: the library prefers IFFT, so does the name. */
    assert(nrf_cs_distance_method_of(2000, 2000, 2000, 2000) == NRF_CS_DISTANCE_METHOD_IFFT);
    assert(nrf_cs_distance_method_of(none, 2000, 2000, 2000) == NRF_CS_DISTANCE_METHOD_PHASE);
    /* Nothing, or a "best" that is not the chosen estimate (the library changed its rule): no method, never a guess. */
    assert(nrf_cs_distance_method_of(none, none, none, none) == NRF_CS_DISTANCE_METHOD_NONE);
    assert(nrf_cs_distance_method_of(3806, 9406, 3135, none) == NRF_CS_DISTANCE_METHOD_NONE);
    assert(nrf_cs_distance_method_of(3806, 9406, 3135, 5000) == NRF_CS_DISTANCE_METHOD_NONE);
    assert(nrf_cs_distance_method_of(3806, 9406, 3135, 9406) == NRF_CS_DISTANCE_METHOD_NONE);  /* phase equals best, IFFT exists */
    assert(nrf_cs_distance_method_of(none, 4638, 3164, 3164) == NRF_CS_DISTANCE_METHOD_NONE);
}

static void test_the_method_survives_the_attempt_record(void)
{
    static const nrf_cs_distance_method_t all[] = {
        NRF_CS_DISTANCE_METHOD_NONE, NRF_CS_DISTANCE_METHOD_IFFT, NRF_CS_DISTANCE_METHOD_PHASE, NRF_CS_DISTANCE_METHOD_RTT,
    };
    static const char *const names[] = { "none", "ifft", "phase", "rtt" };

    for (size_t i = 0U; i < sizeof(all) / sizeof(all[0]); ++i) {
        const uint16_t flags = nrf_cs_distance_method_flag(all[i]);
        assert(nrf_cs_distance_method_from_flags(flags) == all[i]);
        assert(strcmp(nrf_cs_distance_method_name(all[i]), names[i]) == 0);
        /* The other provider bits (realtime RAS, tone ok, PBR, RTT valid: bits 0..3) do not disturb it. */
        assert(nrf_cs_distance_method_from_flags((uint16_t)(flags | 0x0FU)) == all[i]);
        assert(((unsigned)flags & 0x0FU) == 0U);
    }
    /* Two method bits cannot come from one attempt: reported as none, not as one of them. */
    assert(nrf_cs_distance_method_from_flags(NRF_CS_PROVIDER_METHOD_IFFT | NRF_CS_PROVIDER_METHOD_RTT) ==
           NRF_CS_DISTANCE_METHOD_NONE);
    assert(nrf_cs_distance_method_from_flags(NRF_CS_PROVIDER_METHOD_MASK) == NRF_CS_DISTANCE_METHOD_NONE);
    assert(strcmp(nrf_cs_distance_method_name((nrf_cs_distance_method_t)99), "none") == 0);
}

/* The ESP32 prints every attempt record with the method (RANGING_SAMPLE in the ESP log). */
static void test_the_esp_line_of_an_attempt_record_names_the_method(void)
{
    char line[128];
    nrf_cs_cycle_record_t record = {
        .execution = 3U, .attempt = 2U, .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 6194, .confidence = 755U,
        .provider_quality_flags = (uint16_t)(0x0EU | NRF_CS_PROVIDER_METHOD_IFFT),
    };

    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0);
    assert(strcmp(line, "exec=3 attempt=2 outcome=SUCCESS dist_mm=6194 conf=755 method=ifft tone_ok=1 pbr=1 rtt=1") == 0);

    record.provider_quality_flags = (uint16_t)(0x02U | NRF_CS_PROVIDER_METHOD_PHASE);
    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0);
    assert(strstr(line, "method=phase tone_ok=1 pbr=0 rtt=0") != NULL);

    record.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
    record.distance_mm = 0;
    record.confidence = 0U;
    record.provider_quality_flags = 0U;
    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0);
    assert(strcmp(line, "exec=3 attempt=2 outcome=TRANSPORT_ERROR dist_mm=0 conf=0 method=none tone_ok=0 pbr=0 rtt=0") == 0);

    record.outcome = NRF_CS_OUTCOME_TIMEOUT;
    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0 && strstr(line, "outcome=TIMEOUT") != NULL);
    record.outcome = NRF_CS_OUTCOME_OUTLIER;
    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0 && strstr(line, "outcome=OUTLIER") != NULL);
    record.outcome = 77U;
    assert(nrf_cs_cycle_record_describe(&record, line, sizeof(line)) > 0 && strstr(line, "outcome=?") != NULL);

    /* The widest record fits the 112 byte buffer of the ESP32 log. */
    char wide[112];
    record.execution = 255U;
    record.attempt = 255U;
    record.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
    record.distance_mm = -2147483647;
    record.confidence = 65535U;
    record.provider_quality_flags = 0xFFFFU;
    const int length = nrf_cs_cycle_record_describe(&record, wide, sizeof(wide));
    assert(length > 0 && (size_t)length < sizeof(wide));

    assert(nrf_cs_cycle_record_describe(NULL, line, sizeof(line)) < 0);
    assert(nrf_cs_cycle_record_describe(&record, NULL, sizeof(line)) < 0);
    assert(nrf_cs_cycle_record_describe(&record, line, 0U) < 0);
}

/* The record survives the wire: the method bits travel in the 16 bit provider flags of the 12 byte record. */
static void test_the_method_bits_cross_the_result_frame(void)
{
    const uint16_t methods[] = { 0U, 0x10U, 0x20U, 0x40U, 0x50U };
    const char *const names[] = { "none", "ifft", "phase", "rtt", "none" };
    for (size_t i = 0U; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        nrf_cs_measurement_t measurement = {
            .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 3556, .confidence = 999U,
            .provider_quality_flags = (uint16_t)(0x800FU | methods[i]),
        };
        nrf_cs_cycle_result_t result = {
            .request_id = 1U, .cycle_id = 7U, .page_count = 1U, .record_total = 1U, .record_count = 1U,
        };
        nrf_cs_cycle_result_t decoded;
        nrf_cs_measurement_t back;
        uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH];
        char line[112];

        nrf_cs_cycle_record_from_measurement(1U, 4U, &measurement, &result.records[0]);
        assert(nrf_cs_cycle_encode_result(&result, frame) == 0);
        /* Independent wire oracle: first record begins at 18; provider flags are little endian at +10. */
        assert(frame[28] == (uint8_t)(0x0FU | methods[i]) && frame[29] == 0x80U);
        assert(nrf_cs_cycle_decode_result(frame, sizeof(frame), &decoded) == 0);
        nrf_cs_cycle_record_to_measurement(&decoded.records[0], &back);
        assert(back.provider_quality_flags == measurement.provider_quality_flags);
        assert(back.distance_mm == 3556 && back.confidence == 999U);
        assert(nrf_cs_cycle_record_describe(&decoded.records[0], line, sizeof(line)) > 0);
        char expected[24];
        (void)snprintf(expected, sizeof(expected), "method=%s ", names[i]);
        assert(strstr(line, expected) != NULL);
    }
}

int main(void)
{
    test_metres_become_millimetres_or_unavailable();
    test_the_method_follows_the_rule_of_the_library();
    test_the_method_survives_the_attempt_record();
    test_the_esp_line_of_an_attempt_record_names_the_method();
    test_the_method_bits_cross_the_result_frame();
    return 0;
}
