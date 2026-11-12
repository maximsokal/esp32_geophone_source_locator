#define main ble6_replay_original_main
#include "ble6_research_replay.c"
#undef main

static void policy(float ifft, float phase, float rtt, float best, float peak,
                   const uint16_t counts[75], bool tone_ok, bool quality_ok, int32_t out[5])
{
    ble6_replay_firmware_policy(ifft, phase, rtt, best, peak, counts, tone_ok, quality_ok, 512U, out);
}

int main(void)
{
    uint16_t counts[75], saved[75];
    for (size_t i = 0; i < 75; ++i) counts[i] = 1;
    memcpy(saved, counts, sizeof(counts));
    int32_t out[5];
    const float ifft = 3.676f, phase = 5.0f, rtt = 3.389f, best = ifft;
    policy(ifft, phase, rtt, best, 60.0f, counts, true, true, out);
    assert(out[0] == 1876 && out[1] == 400 && out[2] == NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE);
    assert(out[3] == NRF_CS_DISTANCE_METHOD_IFFT && out[4] == 1);
    assert(ifft == 3.676f && phase == 5.0f && rtt == 3.389f && best == ifft);
    assert(memcmp(counts, saved, sizeof(counts)) == 0);

    const float grid = 10.0f * NRF_CS_IFFT_SPEED_OF_LIGHT_MPS / (2.0f * 512.0f * NRF_CS_IFFT_TONE_SPACING_HZ);
    policy(grid, grid, grid, grid, 60.0f, counts, true, true, out);
    assert(out[4] == 1 && out[1] == 400 && out[2] == NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED);
    for (size_t i = 0; i < 30; ++i) counts[i] = 0;
    policy(3.676f, 3.656f, NAN, 3.676f, 60.0f, counts, true, true, out);
    assert(out[4] == 1 && out[1] < 500 && out[2] == NRF_CS_CONFIDENCE_REASON_WIDE_GAP);
    memcpy(counts, saved, sizeof(counts));

    policy(1.800f, 1.810f, NAN, 1.800f, 60.0f, counts, true, true, out);
    assert(out[4] == 0 && out[0] == NRF_CS_DISTANCE_UNAVAILABLE && out[1] == 0);
    policy(1.801f, 1.810f, NAN, 1.801f, 60.0f, counts, true, true, out);
    assert(out[4] == 1 && out[0] == 1 && out[3] == NRF_CS_DISTANCE_METHOD_IFFT);
    policy(NAN, 3.552f, NAN, 3.552f, 60.0f, counts, true, true, out);
    assert(out[4] == 1 && out[0] == 1752 && out[3] == NRF_CS_DISTANCE_METHOD_PHASE);
    policy(NAN, NAN, 2.996f, NAN, NAN, counts, false, true, out);
    assert(out[4] == 1 && out[0] == 2996 && out[3] == NRF_CS_DISTANCE_METHOD_RTT);
    policy(NAN, NAN, 0.0f, 0.0f, NAN, counts, false, true, out);
    assert(out[4] == 0 && out[0] == NRF_CS_DISTANCE_UNAVAILABLE && out[1] == 0);
    policy(3.676f, 3.656f, 3.389f, 3.676f, 60.0f, counts, true, false, out);
    assert(out[4] == 0 && out[1] == 0);
    policy(NAN, NAN, NAN, NAN, NAN, counts, true, true, out);
    assert(out[4] == 0 && out[1] == 0);
    policy(INFINITY, NAN, NAN, INFINITY, NAN, counts, true, true, out);
    assert(out[4] == 0 && out[1] == 0);
    policy(-2.0f, NAN, NAN, -2.0f, NAN, counts, true, true, out);
    assert(out[4] == 0 && out[1] == 0);
    return 0;
}
