#include "localization_channel_health.h"

#include <math.h>
#include <string.h>

#define BLOCK 512U

static const uint8_t PAIR_A[LOCALIZATION_CHANNEL_HEALTH_PAIRS] = {0U, 0U, 0U, 1U, 1U, 2U};
static const uint8_t PAIR_B[LOCALIZATION_CHANNEL_HEALTH_PAIRS] = {1U, 2U, 3U, 2U, 3U, 3U};

int localization_channel_health_pair(size_t pair, size_t *channel_a, size_t *channel_b)
{
    if (pair >= LOCALIZATION_CHANNEL_HEALTH_PAIRS) {
        return 0;
    }
    if (channel_a != NULL) *channel_a = PAIR_A[pair];
    if (channel_b != NULL) *channel_b = PAIR_B[pair];
    return 1;
}

static double mean_of(const float *samples, size_t count)
{
    double sum = 0.0;
    for (size_t first = 0U; first < count; first += BLOCK) {
        const size_t last = first + BLOCK < count ? first + BLOCK : count;
        float block = 0.0f;
        for (size_t i = first; i < last; ++i) block += samples[i];
        sum += (double)block;
    }
    return sum / (double)count;
}

void localization_channel_health_analyze(
    const float *const channel_uv[LOCALIZATION_CHANNEL_HEALTH_CHANNELS],
    size_t count,
    localization_channel_health_t *health_out)
{
    if (health_out == NULL) return;
    memset(health_out, 0, sizeof(*health_out));
    if (channel_uv == NULL || count == 0U) return;

    size_t sensors = 0U;
    for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) {
        if (channel_uv[c] != NULL) ++sensors;
    }
    if (sensors < 2U) return;

    float mean[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
    double energy[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
    float peak[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
    double residual_energy[LOCALIZATION_CHANNEL_HEALTH_PAIRS] = {0};
    for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) {
        if (channel_uv[c] != NULL) mean[c] = (float)mean_of(channel_uv[c], count);
    }

    for (size_t first = 0U; first < count; first += BLOCK) {
        const size_t last = first + BLOCK < count ? first + BLOCK : count;
        float block_energy[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
        float block_residual[LOCALIZATION_CHANNEL_HEALTH_PAIRS] = {0};
        for (size_t i = first; i < last; ++i) {
            float deviation[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
            for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) {
                if (channel_uv[c] == NULL) continue;
                deviation[c] = channel_uv[c][i] - mean[c];
                block_energy[c] += deviation[c] * deviation[c];
                const float magnitude = fabsf(deviation[c]);
                if (magnitude > peak[c]) peak[c] = magnitude;
            }
            for (size_t p = 0U; p < LOCALIZATION_CHANNEL_HEALTH_PAIRS; ++p) {
                if (channel_uv[PAIR_A[p]] == NULL || channel_uv[PAIR_B[p]] == NULL) continue;
                const float diff = deviation[PAIR_A[p]] - deviation[PAIR_B[p]];
                block_residual[p] += diff * diff;
            }
        }
        for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) energy[c] += (double)block_energy[c];
        for (size_t p = 0U; p < LOCALIZATION_CHANNEL_HEALTH_PAIRS; ++p) residual_energy[p] += (double)block_residual[p];
    }

    float rms[LOCALIZATION_CHANNEL_HEALTH_CHANNELS] = {0};
    for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) {
        if (channel_uv[c] != NULL) rms[c] = (float)sqrt(energy[c] / (double)count);
    }

    for (size_t p = 0U; p < LOCALIZATION_CHANNEL_HEALTH_PAIRS; ++p) {
        const size_t a = PAIR_A[p];
        const size_t b = PAIR_B[p];
        if (channel_uv[a] == NULL || channel_uv[b] == NULL) continue;
        const float weaker = rms[a] < rms[b] ? rms[a] : rms[b];
        if (weaker < LOCALIZATION_CHANNEL_HEALTH_ACTIVE_RMS_UV) continue;
        const float residual_rms = (float)sqrt(residual_energy[p] / (double)count);
        if (residual_rms <= LOCALIZATION_CHANNEL_HEALTH_IDENTICAL_RESIDUAL * weaker) {
            health_out->identical_pair_mask = (uint8_t)(health_out->identical_pair_mask | (1U << p));
        }
    }

    for (size_t c = 0U; c < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++c) {
        if (channel_uv[c] == NULL || peak[c] >= LOCALIZATION_CHANNEL_HEALTH_FLAT_PEAK_UV) continue;
        float strongest = 0.0f;
        for (size_t other = 0U; other < LOCALIZATION_CHANNEL_HEALTH_CHANNELS; ++other) {
            if (other != c && channel_uv[other] != NULL && peak[other] > strongest) strongest = peak[other];
        }
        const float needed = LOCALIZATION_CHANNEL_HEALTH_FLAT_NEIGHBOUR_RATIO * peak[c];
        if (strongest >= LOCALIZATION_CHANNEL_HEALTH_ACTIVE_RMS_UV * 2.0f && strongest >= needed) {
            health_out->flat_channel_mask = (uint8_t)(health_out->flat_channel_mask | (1U << c));
        }
    }
}
