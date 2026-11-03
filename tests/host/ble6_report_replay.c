/* Replay exported per-channel reports, not original HCI/RAS packets. */
#define main component_replay_main
#include "ble6_research_replay.c"
#undef main

int main(void)
{
    FILE *in = fopen(getenv("BLE6_REPORT_INPUT"), "rb");
    FILE *out = fopen(getenv("BLE6_REPORT_OUTPUT"), "wb");
    if (in == NULL || out == NULL) return 2;
    size_t n = 0;
    float values[300], scratch[1024], result[10];
    uint16_t counts[75];
    int32_t accumulated;
    uint8_t rtt_count, tone_ok;
    while (fread(values, sizeof(values), 1, in) == 1U) {
        if (fread(counts, sizeof(counts), 1, in) != 1U ||
            fread(&accumulated, sizeof(accumulated), 1, in) != 1U ||
            fread(&rtt_count, 1, 1, in) != 1U || fread(&tone_ok, 1, 1, in) != 1U) return 3;
        cs_de_report_t report = {0};
        report.n_ap = 1U;
        memcpy(&report.iq_tones[0], values, sizeof(values));
        report.rtt_accumulated_half_ns = accumulated;
        report.rtt_count = rtt_count;
        report.tone_quality[0] = tone_ok ? CS_DE_TONE_QUALITY_OK : CS_DE_TONE_QUALITY_BAD;
        report.distance_estimates[0] = (cs_de_dist_estimates_t){NAN, NAN, NAN, NAN};
        const cs_de_quality_t quality = cs_de_calc(&report);
        result[0] = report.distance_estimates[0].ifft;
        result[1] = report.distance_estimates[0].phase_slope;
        result[2] = report.distance_estimates[0].rtt;
        result[3] = report.distance_estimates[0].best;
        float selected = result[3], filled = result[0], sharp = NAN;
        uint16_t good = 0;
        for (size_t i = 0; i < 75; ++i) good += counts[i] ? 1U : 0U;
        if (tone_ok && isfinite(result[0]) && selected == result[0]) {
            memset(scratch, 0, sizeof(scratch));
            cs_de_combined_iq_calculate(&report.iq_tones[0], scratch);
            const size_t changed = nrf_cs_iq_gap_fill(scratch, counts, 75U);
            const float candidate = cs_de_ifft(scratch);
            if (changed && isfinite(candidate) && candidate >= 0.0f) filled = selected = candidate;
            sharp = nrf_cs_ifft_peak_to_floor(scratch, 512U);
        }
        if (quality == CS_DE_QUALITY_OK && !tone_ok && !isfinite(selected) &&
            isfinite(result[2]) && result[2] > 0.0f) selected = result[2];
        if (!isfinite(result[0]) && !isfinite(result[1]) &&
            (!isfinite(result[2]) || result[2] <= 0.0f)) selected = NAN;
        const bool pbr = isfinite(result[0]) || isfinite(result[1]);
        const bool rtt_valid = isfinite(result[2]) && result[2] > 0.0f;
        result[4] = selected; result[5] = filled; result[6] = sharp;
        result[7] = (float)nrf_cs_measurement_confidence(good, 15, pbr, rtt_valid, selected, result[2]);
        result[8] = (float)nrf_cs_measurement_confidence_with_peak(good, 15, pbr, rtt_valid, selected, result[2], sharp);
        result[9] = (float)quality;
        if (fwrite(result, sizeof(result), 1, out) != 1U) return 4;
        ++n;
    }
    fclose(in); fclose(out);
    printf("cs_de_calc_reports=%zu\n", n);
    return 0;
}
