#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_cs_distance_method.h"
#include "nrf_cs_tone_stats.h"
#include "nrf_cs_iq_gap_fill.h"
#include "nrf_cs_ifft_quality.h"

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

/* CS-IFFT-GAP-01: the numbers that tell whether the IFFT was fed a frequency set with holes. */
static void test_tone_stats_count_usable_tones_and_the_longest_hole(void)
{
    bool planned[75];
    bool usable[75];

    for (size_t i = 0U; i < 75U; ++i) { planned[i] = true; usable[i] = true; }
    nrf_cs_tone_stats_t s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.planned == 75U && s.usable == 75U && s.longest_gap == 0U);

    /* Scattered single losses: 3 usable tones missing, no hole longer than one slot. */
    usable[10] = usable[40] = usable[70] = false;
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.usable == 72U && s.longest_gap == 1U);

    /* One continuous hole of eight slots (the shape that produced a side lobe on DevZone) and two shorter ones. */
    for (size_t i = 20U; i < 28U; ++i) usable[i] = false;
    usable[50] = usable[51] = false;
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.usable == 62U && s.longest_gap == 8U);

    /* A hole at either end counts too: the first and last slots hold values as well. */
    for (size_t i = 0U; i < 75U; ++i) usable[i] = true;
    usable[0] = usable[1] = usable[2] = false;
    assert(nrf_cs_tone_stats(planned, usable, 75U).longest_gap == 3U);
    for (size_t i = 0U; i < 75U; ++i) usable[i] = true;
    usable[73] = usable[74] = false;
    assert(nrf_cs_tone_stats(planned, usable, 75U).longest_gap == 2U);

    /* Channels outside the map are zeros as well: they extend a hole, but they are not "missing" tones. */
    for (size_t i = 0U; i < 75U; ++i) { planned[i] = i < 60U; usable[i] = i < 60U; }
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.planned == 60U && s.usable == 60U && s.longest_gap == 15U);
    /* A tone that was good but sits on an unplanned channel does not count as usable. */
    usable[70] = true;
    assert(nrf_cs_tone_stats(planned, usable, 75U).usable == 60U);

    /* Where the hole is: a Wi-Fi channel 1 access point covers the first ~20 slots, an edge hole, not a hole in the middle. */
    for (size_t i = 0U; i < 75U; ++i) { planned[i] = true; usable[i] = i >= 20U; }
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.longest_gap == 20U && s.lead_gap == 20U && s.tail_gap == 0U && s.usable == 55U);
    for (size_t i = 0U; i < 75U; ++i) usable[i] = i < 60U;
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.longest_gap == 15U && s.lead_gap == 0U && s.tail_gap == 15U);
    for (size_t i = 0U; i < 75U; ++i) usable[i] = !(i >= 30U && i < 40U);
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.longest_gap == 10U && s.lead_gap == 0U && s.tail_gap == 0U);          /* in the middle: neither end */

    /* Nothing usable at all: the whole array is one hole. */
    for (size_t i = 0U; i < 75U; ++i) { planned[i] = true; usable[i] = false; }
    s = nrf_cs_tone_stats(planned, usable, 75U);
    assert(s.usable == 0U && s.longest_gap == 75U && s.lead_gap == 75U && s.tail_gap == 75U);

    s = nrf_cs_tone_stats(NULL, usable, 75U);
    assert(s.planned == 0U && s.usable == 0U && s.longest_gap == 0U);
    s = nrf_cs_tone_stats(planned, usable, 300U);                    /* more than the counters hold */
    assert(s.planned == 0U);
}

/* Interior complex tones must follow phase through the +/-pi branch and geometric magnitude. */
static void test_gap_fill_preserves_observations_and_leaves_edges_missing(void)
{
    float iq[14] = {0,0, -1.0f,0.0f, 0,0, 0,0, 0,-8.0f, 0,0, 0,0};
    const uint16_t counts[7] = {0,1,0,0,1,0,0};
    assert(nrf_cs_iq_gap_fill(iq, counts, 7U) == 2U);
    assert(fabsf(iq[4] - (-1.7320508f)) < 0.0001f && fabsf(iq[5] - (-1.0f)) < 0.0001f);
    assert(fabsf(iq[6] - (-2.0f)) < 0.0001f && fabsf(iq[7] - (-3.4641016f)) < 0.0001f);
    assert(iq[0] == 0.0f && iq[1] == 0.0f && iq[10] == 0.0f && iq[13] == 0.0f);
    assert(iq[2] == -1.0f && iq[3] == 0.0f && iq[8] == 0.0f && iq[9] == -8.0f);
}

/* Independent DFT check: a 20-tone hole creates an early lobe above Nordic's 40% threshold. */
static void test_gap_fill_suppresses_synthetic_false_early_peak(void)
{
    const double pi = 3.14159265358979323846;
    float iq[150];
    uint16_t counts[75];
    double spectrum[512];
    for (size_t i = 0U; i < 75U; ++i) {
        const double phase = -4.0 * pi * 1000000.0 * 3.0 * (double)i / 299792458.0;
        counts[i] = i >= 25U && i < 45U ? 0U : 1U;
        iq[2U*i] = counts[i] ? (float)cos(phase) : 0.0f;
        iq[2U*i+1U] = counts[i] ? (float)sin(phase) : 0.0f;
    }
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        if (pass == 1U) assert(nrf_cs_iq_gap_fill(iq, counts, 75U) == 20U);
        size_t strongest = 0U;
        for (size_t k = 0U; k < 512U; ++k) {
            double re = 0.0, im = 0.0;
            for (size_t n = 0U; n < 75U; ++n) {
                const double phase = 2.0 * pi * (double)(k*n) / 512.0;
                re += iq[2U*n] * cos(phase) - iq[2U*n+1U] * sin(phase);
                im += iq[2U*n] * sin(phase) + iq[2U*n+1U] * cos(phase);
            }
            spectrum[k] = hypot(re, im);
            if (spectrum[k] > spectrum[strongest]) strongest = k;
        }
        assert(strongest == 10U);
        bool early = false;
        for (size_t k = 1U; k < strongest; ++k) {
            if (spectrum[k] > spectrum[k-1U] && spectrum[k] > spectrum[k+1U] &&
                spectrum[k] * 2.5 > spectrum[strongest]) early = true;
        }
        assert(early == (pass == 0U));
    }
}

static void test_gap_fill_leaves_wide_holes_and_zero_endpoints(void)
{
    float iq[68] = {0}; uint16_t counts[34] = {0};
    counts[0] = counts[31] = 1U; iq[0] = iq[62] = 1.0f;
    assert(nrf_cs_iq_gap_fill(iq, counts, 34U) == 0U);
    counts[31] = 0U; counts[2] = 1U; iq[4] = 0.0f;
    assert(nrf_cs_iq_gap_fill(iq, counts, 34U) == 0U);
    assert(nrf_cs_iq_gap_fill(NULL, counts, 34U) == 0U);
}

/* A ten-metre phase rotates by more than pi across a 20-channel hole. */
static void test_gap_fill_uses_observed_slope_to_unwrap_a_wide_hole(void)
{
    const double pi = 3.14159265358979323846;
    float iq[150]; uint16_t counts[75];
    for (size_t i = 0U; i < 75U; ++i) {
        const double phase = -4.0 * pi * 1000000.0 * 10.0 * (double)i / 299792458.0;
        counts[i] = i >= 25U && i < 45U ? 0U : 1U;
        iq[2U*i] = counts[i] ? (float)cos(phase) : 0.0f;
        iq[2U*i+1U] = counts[i] ? (float)sin(phase) : 0.0f;
    }
    assert(nrf_cs_iq_gap_fill(iq, counts, 75U) == 20U);
    for (size_t i = 25U; i < 45U; ++i) {
        const double phase = -4.0 * pi * 1000000.0 * 10.0 * (double)i / 299792458.0;
        assert(fabs(iq[2U*i] - cos(phase)) < 0.0001);
        assert(fabs(iq[2U*i+1U] - sin(phase)) < 0.0001);
    }
}

/* Real Wi-Fi max procedure 2712 has a 24-slot gap; do not leave it zero-filled. */
static void test_gap_fill_handles_24_channel_wifi_hole(void)
{
    float iq[60] = {0}; uint16_t counts[30] = {0};
    for (size_t i = 0U; i < 30U; ++i) {
        if (i >= 3U && i <= 26U) continue;
        counts[i] = 1U; iq[2U*i] = 1.0f;
    }
    assert(nrf_cs_iq_gap_fill(iq, counts, 30U) == 24U);
    for (size_t i = 3U; i <= 26U; ++i) assert(fabsf(iq[2U*i] - 1.0f) < 0.0001f);
}

/* Peak over median of an IFFT magnitude profile: the sharpness the log reports for every attempt. */
static void test_peak_to_floor_of_a_magnitude_profile(void)
{
    float flat[512];
    for (size_t i = 0U; i < 512U; ++i) flat[i] = 1.0f;
    flat[100] = 50.0f;
    assert(fabsf(nrf_cs_ifft_peak_to_floor(flat, 512U) - 50.0f) < 1e-4f);

    /* Two comparable paths: the ratio does not care where they are, only how much stronger the peak is than the floor. */
    float two[512];
    for (size_t i = 0U; i < 512U; ++i) two[i] = 2.0f;
    two[20] = 30.0f; two[400] = 28.0f;
    assert(fabsf(nrf_cs_ifft_peak_to_floor(two, 512U) - 15.0f) < 1e-4f);

    /* An even count takes the mean of the two middle values, like the analysis the threshold comes from. */
    float even[4] = {1.0f, 2.0f, 4.0f, 8.0f};
    assert(fabsf(nrf_cs_ifft_peak_to_floor(even, 4U) - 8.0f / 3.0f) < 1e-5f);
    float odd[5] = {9.0f, 1.0f, 5.0f, 3.0f, 7.0f};
    assert(fabsf(nrf_cs_ifft_peak_to_floor(odd, 5U) - 9.0f / 5.0f) < 1e-5f);

    /* The selection agrees with a full sort on awkward input: duplicates, descending, many equal values. */
    float many[512];
    for (size_t i = 0U; i < 512U; ++i) many[i] = (float)((i * 37U) % 7U) + (i == 311U ? 40.0f : 0.0f);
    float sorted[512];
    for (size_t i = 0U; i < 512U; ++i) sorted[i] = many[i];
    for (size_t i = 1U; i < 512U; ++i) {
        const float key = sorted[i]; size_t j = i;
        while (j > 0U && sorted[j - 1U] > key) { sorted[j] = sorted[j - 1U]; --j; }
        sorted[j] = key;
    }
    const float expected = sorted[511] / (0.5f * (sorted[255] + sorted[256]));
    assert(fabsf(nrf_cs_ifft_peak_to_floor(many, 512U) - expected) < 1e-4f);
    float descending[64];
    for (size_t i = 0U; i < 64U; ++i) descending[i] = (float)(64U - i);
    assert(fabsf(nrf_cs_ifft_peak_to_floor(descending, 64U) - 64.0f / 32.5f) < 1e-5f);

    /* Nothing to measure: not a number, never a made-up ratio. */
    float zero[8] = {0};
    assert(isnan(nrf_cs_ifft_peak_to_floor(zero, 8U)));
    float bad[8] = {1, 1, 1, NAN, 1, 1, 1, 1};
    assert(isnan(nrf_cs_ifft_peak_to_floor(bad, 8U)));
    float negative[8] = {1, 1, 1, -1, 1, 1, 1, 1};
    assert(isnan(nrf_cs_ifft_peak_to_floor(negative, 8U)));
    assert(isnan(nrf_cs_ifft_peak_to_floor(NULL, 8U)));
    assert(isnan(nrf_cs_ifft_peak_to_floor(odd, 1U)));
    assert(NRF_CS_IFFT_SHARP_PEAK_RATIO == 10.0f);
}

int main(void)
{
    test_gap_fill_preserves_observations_and_leaves_edges_missing();
    test_gap_fill_suppresses_synthetic_false_early_peak();
    test_gap_fill_handles_24_channel_wifi_hole();
    test_gap_fill_uses_observed_slope_to_unwrap_a_wide_hole();
    test_gap_fill_leaves_wide_holes_and_zero_endpoints();
    test_tone_stats_count_usable_tones_and_the_longest_hole();
    test_peak_to_floor_of_a_magnitude_profile();
    test_metres_become_millimetres_or_unavailable();
    test_the_method_follows_the_rule_of_the_library();
    test_the_method_survives_the_attempt_record();
    test_the_esp_line_of_an_attempt_record_names_the_method();
    test_the_method_bits_cross_the_result_frame();
    return 0;
}
