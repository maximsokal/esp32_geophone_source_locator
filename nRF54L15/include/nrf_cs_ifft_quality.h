#ifndef NRF_CS_IFFT_QUALITY_H
#define NRF_CS_IFFT_QUALITY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How sharp the IFFT peak is: the strongest bin over the median bin of the same IFFT magnitude profile.
 *
 * cs_de_ifft() leaves its magnitude profile in the first NFFT floats of the buffer it was given. This helper reorders that profile.
 * The strongest bin may belong to a later path than the SDK-selected distance. A coherent reflected path can also have a high
 * ratio; this is spectral contrast, not evidence of a direct path or calibrated distance accuracy.
 *
 * The confidence policy currently uses this ratio. Earlier 44% availability/accuracy claims predate paired-product averaging:
 * current Wieme replay accepts 21297/21370 finite distances, including 3029 errors >2 m without an offset correction.
 * See docs/testing/ble6-b5-confidence-diagnostics-2026-11-04.md. The threshold remains an unvalidated policy assumption.
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
