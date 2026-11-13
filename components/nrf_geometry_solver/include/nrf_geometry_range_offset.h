#pragma once

#include "nrf_geometry_solver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t offset_mm;
    size_t count;
    int64_t span_mm;
} nrf_geometry_range_offset_t;

/* Eligible manual IFFT pairs contribute automatic minus manual distance. */
bool nrf_geometry_range_offset_estimate(const nrf_cs_pair_result_t *pairs, size_t pair_count,
                                        nrf_geometry_range_offset_t *out);

/* The output must not overlap pairs. Invalid corrected distances are excluded. */
bool nrf_geometry_range_offset_apply(const nrf_cs_pair_result_t *pairs, size_t pair_count,
                                     const nrf_geometry_node_input_t *nodes, size_t node_count,
                                     int32_t offset_mm, nrf_cs_pair_result_t *out);

/*
 * Degraded pairs. A pair whose samples all fell below min_confidence ends RETRY_EXHAUSTED and leaves the geometry
 * with nothing to solve (board log 2026-11-13: node 1 behind an obstacle, RSSI 10-18 dB below the others, so 1-3 and
 * 1-4 were dropped and nodes 3 and 4 stayed unsolved). Such a pair is accepted, marked NRF_CS_QUALITY_DEGRADED and
 * given a low confidence, when it is the best evidence there is:
 *   - both directions produced at least NRF_GEOMETRY_DEGRADED_MIN_SAMPLES samples,
 *   - the two direction medians agree within max(NRF_GEOMETRY_DEGRADED_AGREE_MM, NRF_GEOMETRY_DEGRADED_AGREE_PERCENT %),
 *   - no triangle it belongs to violates the triangle inequality by more than NRF_GEOMETRY_DEGRADED_TRIANGLE_SLACK_MM
 *     (then every degraded side of that triangle is left out; a valid side is never touched).
 * Its distance is the mean of the two direction medians. Returns the number of pairs promoted.
 */
#define NRF_GEOMETRY_DEGRADED_MIN_SAMPLES 2U
#define NRF_GEOMETRY_DEGRADED_AGREE_MM 500
#define NRF_GEOMETRY_DEGRADED_AGREE_PERCENT 15
#define NRF_GEOMETRY_DEGRADED_TRIANGLE_SLACK_MM 300
#define NRF_GEOMETRY_DEGRADED_CONFIDENCE 300U
size_t nrf_geometry_degraded_pairs_promote(nrf_cs_pair_result_t *pairs, size_t pair_count);

#ifdef __cplusplus
}
#endif
