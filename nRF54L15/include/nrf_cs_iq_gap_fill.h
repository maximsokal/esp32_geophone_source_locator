#ifndef NRF_CS_IQ_GAP_FILL_H
#define NRF_CS_IQ_GAP_FILL_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Holes of this many slots or more are not filled (an engineering limit, not a measured value). */
#define NRF_CS_IQ_GAP_FILL_LIMIT 30U

/* Nordic DevZone case 129386: interpolate interior combined IQ in log magnitude
 * and phase; unwrap with the measured adjacent-tone slope, never extrapolate edges.
 * shortcut: gaps of 30 MHz or wider remain unfilled until wide-gap multipath behavior is validated. */
static inline size_t nrf_cs_iq_gap_fill(float *iq, const uint16_t *counts, size_t channels)
{
    const float pi = 3.14159265358979323846f;
    size_t filled = 0U;
    size_t left = 0U;
    if (iq == NULL || counts == NULL) return 0U;
    float slope_re = 0.0f, slope_im = 0.0f;
    for (size_t i = 1U; i < channels; ++i) {
        if (counts[i - 1U] == 0U || counts[i] == 0U) continue;
        const float ar = iq[2U * (i - 1U)], ai = iq[2U * (i - 1U) + 1U];
        const float br = iq[2U * i], bi = iq[2U * i + 1U];
        const float norm = hypotf(ar, ai) * hypotf(br, bi);
        if (!isfinite(norm) || norm <= 0.0f) continue;
        slope_re += (ar * br + ai * bi) / norm;
        slope_im += (ar * bi - ai * br) / norm;
    }
    const bool have_slope = isfinite(slope_re) && isfinite(slope_im) && (slope_re != 0.0f || slope_im != 0.0f);
    const float slope = have_slope ? atan2f(slope_im, slope_re) : 0.0f;
    while (left < channels) {
        if (counts[left] == 0U) { ++left; continue; }
        size_t right = left + 1U;
        while (right < channels && counts[right] == 0U) ++right;
        if (right == channels) break;
        const size_t gap = right - left - 1U;
        if (gap > 0U && gap < NRF_CS_IQ_GAP_FILL_LIMIT) {
            const float a = hypotf(iq[2U * left], iq[2U * left + 1U]);
            const float b = hypotf(iq[2U * right], iq[2U * right + 1U]);
            if (isfinite(a) && isfinite(b) && a > 0.0f && b > 0.0f) {
                const float phase = atan2f(iq[2U * left + 1U], iq[2U * left]);
                float delta = atan2f(iq[2U * right + 1U], iq[2U * right]) - phase;
                if (delta > pi) delta -= 2.0f * pi;
                if (delta < -pi) delta += 2.0f * pi;
                if (have_slope) delta += 2.0f * pi * roundf((slope * (float)(gap + 1U) - delta) / (2.0f * pi));
                const float log_a = logf(a), log_b = logf(b);
                for (size_t i = 1U; i <= gap; ++i) {
                    const float t = (float)i / (float)(gap + 1U);
                    const float mag = expf(log_a + t * (log_b - log_a));
                    const float angle = phase + t * delta;
                    iq[2U * (left + i)] = mag * cosf(angle);
                    iq[2U * (left + i) + 1U] = mag * sinf(angle);
                    ++filled;
                }
            }
        }
        left = right;
    }
    return filled;
}
#endif
