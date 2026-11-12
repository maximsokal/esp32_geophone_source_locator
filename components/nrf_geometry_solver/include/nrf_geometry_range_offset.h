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

#ifdef __cplusplus
}
#endif
