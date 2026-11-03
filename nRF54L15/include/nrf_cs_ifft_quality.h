#ifndef NRF_CS_IFFT_QUALITY_H
#define NRF_CS_IFFT_QUALITY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How sharp the IFFT peak is: the strongest bin over the median bin of the same IFFT magnitude profile.
 *
 * cs_de_ifft() leaves its magnitude profile in the first NFFT floats of the buffer it was given, so this costs one pass and no extra
 * FFT. A clean direct path gives a high ratio; several comparable paths (multipath) or noise give a low one.
 *
 * Why it is logged: on the Wieme replay (records with every legal channel observed, line of sight, 1-5 m, files the threshold was
 * not chosen on) estimates whose ratio exceeds 10 had a median error of 0.12 m and a 95th percentile of 0.58 m after the constant
 * offset was removed, against 0.20 m and 2.35 m for all of them; 44% of the estimates passed. The replay is one dataset and one
 * hardware, so the threshold is a hypothesis for this board, and the ratio is only logged (NRF_CS_IFFT_SHARP_PEAK_RATIO is the
 * value the analysis used). Nothing is rejected on it yet.
 */
#define NRF_CS_IFFT_SHARP_PEAK_RATIO 10.0f

/* The k-th smallest (0-based) of v[0..n-1]; reorders v. Hoare selection, no recursion, no allocation. */
static inline float nrf_cs_ifft_select_kth(float *v, size_t n, size_t k)
{
    int lo = 0, hi = (int)n - 1;
    const int want = (int)k;
    while (lo < hi) {
        const float pivot = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) ++i;
            while (v[j] > pivot) --j;
            if (i <= j) {
                const float swap = v[i]; v[i] = v[j]; v[j] = swap;
                ++i; --j;
            }
        }
        if (want <= j) hi = j;
        else if (want >= i) lo = i;
        else break;
    }
    return v[want];
}

/* Strongest bin / median bin of mag[0..n-1] (n >= 2). Reorders `mag`. NAN when the profile is empty, not finite or has a zero median. */
static inline float nrf_cs_ifft_peak_to_floor(float *mag, size_t n)
{
    if (mag == NULL || n < 2U) return NAN;
    float peak = 0.0f;
    for (size_t i = 0U; i < n; ++i) {
        if (!isfinite(mag[i]) || mag[i] < 0.0f) return NAN;
        if (mag[i] > peak) peak = mag[i];
    }
    const float upper = nrf_cs_ifft_select_kth(mag, n, n / 2U);
    float median = upper;
    if ((n % 2U) == 0U) {
        const float lower = nrf_cs_ifft_select_kth(mag, n, n / 2U - 1U);
        median = 0.5f * (lower + upper);
    }
    return median > 0.0f ? peak / median : NAN;
}

#endif
