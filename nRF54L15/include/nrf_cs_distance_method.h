#ifndef NRF_CS_DISTANCE_METHOD_H
#define NRF_CS_DISTANCE_METHOD_H

#include <math.h>
#include <stdint.h>

/*
 * Which of the three estimates of the Nordic distance library produced the distance of one Channel Sounding attempt.
 *
 * cs_de_calc() (NCS v3.4.0, subsys/bluetooth/cs_de/cs_de.c set_best_estimate) fills three independent estimates and then
 * copies the first finite one into "best": IFFT, else phase slope, else RTT. On the bench the three disagree by metres
 * (2026-11-02: 0->4 IFFT 6.1 m, phase 9.4 m, RTT 4.7 m), so the method behind a distance has to be in the log and in
 * the attempt record, otherwise a bad distance cannot be traced to the method that made it.
 */

/* An estimate the library did not make (not finite, negative): no millimetre value. */
#define NRF_CS_DISTANCE_UNAVAILABLE INT32_MIN

/* Bits of the provider quality flags (uint16, in every attempt record); bits 0..3 are in nrf_cs_pair_runtime_zephyr.h. */
#define NRF_CS_PROVIDER_METHOD_IFFT  (1U << 4)
#define NRF_CS_PROVIDER_METHOD_PHASE (1U << 5)
#define NRF_CS_PROVIDER_METHOD_RTT   (1U << 6)
#define NRF_CS_PROVIDER_METHOD_MASK  (NRF_CS_PROVIDER_METHOD_IFFT | NRF_CS_PROVIDER_METHOD_PHASE | NRF_CS_PROVIDER_METHOD_RTT)

typedef enum {
    NRF_CS_DISTANCE_METHOD_NONE = 0,    /* no estimate, or "best" is not one of the three */
    NRF_CS_DISTANCE_METHOD_IFFT,
    NRF_CS_DISTANCE_METHOD_PHASE,
    NRF_CS_DISTANCE_METHOD_RTT,
} nrf_cs_distance_method_t;

/* Metres of the library to whole millimetres; NRF_CS_DISTANCE_UNAVAILABLE when not finite, negative or absurd. */
static inline int32_t nrf_cs_distance_mm_from_m(float metres)
{
    if (!isfinite(metres) || metres < 0.0f || metres > 2000000.0f) return NRF_CS_DISTANCE_UNAVAILABLE;
    return (int32_t)(metres * 1000.0f + 0.5f);
}

/*
 * The method behind "best", decided the way the library decides (IFFT, then phase slope, then RTT) and accepted only when
 * the chosen estimate really equals "best": a library that changes its rule shows up as NONE, never as a wrong name.
 */
static inline nrf_cs_distance_method_t nrf_cs_distance_method_of(
    int32_t ifft_mm, int32_t phase_mm, int32_t rtt_mm, int32_t best_mm)
{
    if (best_mm == NRF_CS_DISTANCE_UNAVAILABLE) return NRF_CS_DISTANCE_METHOD_NONE;
    if (ifft_mm != NRF_CS_DISTANCE_UNAVAILABLE) {
        return ifft_mm == best_mm ? NRF_CS_DISTANCE_METHOD_IFFT : NRF_CS_DISTANCE_METHOD_NONE;
    }
    if (phase_mm != NRF_CS_DISTANCE_UNAVAILABLE) {
        return phase_mm == best_mm ? NRF_CS_DISTANCE_METHOD_PHASE : NRF_CS_DISTANCE_METHOD_NONE;
    }
    if (rtt_mm != NRF_CS_DISTANCE_UNAVAILABLE) {
        return rtt_mm == best_mm ? NRF_CS_DISTANCE_METHOD_RTT : NRF_CS_DISTANCE_METHOD_NONE;
    }
    return NRF_CS_DISTANCE_METHOD_NONE;
}

static inline uint16_t nrf_cs_distance_method_flag(nrf_cs_distance_method_t method)
{
    switch (method) {
        case NRF_CS_DISTANCE_METHOD_IFFT: return NRF_CS_PROVIDER_METHOD_IFFT;
        case NRF_CS_DISTANCE_METHOD_PHASE: return NRF_CS_PROVIDER_METHOD_PHASE;
        case NRF_CS_DISTANCE_METHOD_RTT: return NRF_CS_PROVIDER_METHOD_RTT;
        default: return 0U;
    }
}

/* From the flags of an attempt record; NONE also when more than one method bit is set. */
static inline nrf_cs_distance_method_t nrf_cs_distance_method_from_flags(uint16_t flags)
{
    switch (flags & NRF_CS_PROVIDER_METHOD_MASK) {
        case NRF_CS_PROVIDER_METHOD_IFFT: return NRF_CS_DISTANCE_METHOD_IFFT;
        case NRF_CS_PROVIDER_METHOD_PHASE: return NRF_CS_DISTANCE_METHOD_PHASE;
        case NRF_CS_PROVIDER_METHOD_RTT: return NRF_CS_DISTANCE_METHOD_RTT;
        default: return NRF_CS_DISTANCE_METHOD_NONE;
    }
}

static inline const char *nrf_cs_distance_method_name(nrf_cs_distance_method_t method)
{
    switch (method) {
        case NRF_CS_DISTANCE_METHOD_IFFT: return "ifft";
        case NRF_CS_DISTANCE_METHOD_PHASE: return "phase";
        case NRF_CS_DISTANCE_METHOD_RTT: return "rtt";
        default: return "none";
    }
}

#endif
