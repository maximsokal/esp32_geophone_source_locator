/* Diagnostic use of installed SDK internals; not production firmware. */
#define main ble6_replay_original_main
#include "ble6_research_replay.c"
#undef main

int main(void)
{
    const char *input = getenv("BLE6_REPLAY_INPUT");
    const char *profile_path = getenv("BLE6_PEAK_PROFILE");
    if (input == NULL || profile_path == NULL) return 2;
    FILE *in = fopen(input, "rb");
    if (in == NULL) return 3;
    uint16_t steps, counts[75];
    float original[150], rtt;
    cs_de_report_t report = {0};
    memset(s_iq_counts, 0, sizeof(s_iq_counts));
    nrf_cs_rf_reset(&s_rf_diagnostics);
    if (fread(&steps, sizeof(steps), 1, in) != 1U) return 4;
    for (uint16_t i=0; i<steps; ++i) {
        uint8_t raw[11];
        if (fread(raw, sizeof(raw), 1, in) != 1U) return 4;
        const struct bt_le_cs_subevent_step local = {2, raw[0], 5, raw+1};
        const struct bt_le_cs_subevent_step peer = {2, raw[0], 5, raw+6};
        collect_pbr_step(&report, &local, &peer);
    }
    if (fread(original, sizeof(original), 1, in) != 1U ||
        fread(counts, sizeof(counts), 1, in) != 1U || fread(&rtt, sizeof(rtt), 1, in) != 1U) return 4;
    fclose(in);
    float buf[2 * CONFIG_BT_CS_DE_NFFT_SIZE] = {0};
    if (steps > 0U) cs_de_combined_iq_calculate(&report.iq_tones[0], buf);
    else memcpy(buf, original, sizeof(original));
    (void)nrf_cs_iq_gap_fill(buf, counts, 75U);
    calculate_ifft_mag(buf);
    float peak;
    uint32_t global;
    arm_max_f32(buf, CONFIG_BT_CS_DE_NFFT_SIZE, &peak, &global);

    /* Mirror Nordic's pre-compensation selection and verify against its actual function. */
    uint32_t nw = CONFIG_BT_CS_DE_NFFT_SIZE - 2U, next = CONFIG_BT_CS_DE_NFFT_SIZE - 1U;
    uint32_t first = global;
    bool rise = false;
    while (nw != global) {
        if (buf[next] < buf[nw]) {
            if (2.5f * buf[nw] > peak && rise) { first = nw; break; }
        } else rise = true;
        nw = next;
        next = (next + 1U) % CONFIG_BT_CS_DE_NFFT_SIZE;
    }
    int32_t left = -1;
    uint32_t compensated = first;
    if (first < CONFIG_BT_CS_DE_NFFT_SIZE - 2U) {
        left = calculate_ifft_find_left_null(first, buf);
        compensated = calculate_left_null_compensation_of_peak(first, buf);
    }
    const uint32_t selected = find_ifft_peak_index(buf);
    if (selected != compensated) return 5;
    const float distance = calculate_ifft_peak_index_to_distance(selected, buf);
    FILE *out = fopen(profile_path, "wb");
    if (out == NULL) return 6;
    if (fwrite(buf, sizeof(float), CONFIG_BT_CS_DE_NFFT_SIZE, out) != CONFIG_BT_CS_DE_NFFT_SIZE) return 7;
    fclose(out);
    printf("{\"nfft\":%u,\"global_index\":%u,\"first_index\":%u,\"left_index\":%d,"
           "\"compensated_index\":%u,\"selected_index\":%u,\"global_amplitude\":%.9g,"
           "\"first_amplitude\":%.9g,\"distance_m\":%.9g}\n", CONFIG_BT_CS_DE_NFFT_SIZE,
           global, first, left, compensated, selected, (double)peak, (double)buf[first], (double)distance);
    return 0;
}
