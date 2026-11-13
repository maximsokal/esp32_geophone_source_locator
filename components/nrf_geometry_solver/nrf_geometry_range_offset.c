#include "nrf_geometry_range_offset.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_distance_method.h"

static bool single_ifft(const nrf_cs_pair_result_t *pair)
{
    return (pair->provider_quality_flags & NRF_CS_PROVIDER_METHOD_MASK) == NRF_CS_PROVIDER_METHOD_IFFT;
}

bool nrf_geometry_range_offset_estimate(const nrf_cs_pair_result_t *pairs, size_t pair_count,
                                        nrf_geometry_range_offset_t *out)
{
    if (pairs == NULL || out == NULL || pair_count == 0 || pair_count > NRF_CS_MAX_PAIRS) {
        return false;
    }
    int64_t offsets[NRF_CS_MAX_PAIRS];
    size_t count = 0;
    for (size_t i = 0; i < pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = &pairs[i];
        if (pair->state != NRF_CS_PAIR_VALID || !(pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) ||
            !single_ifft(pair) || pair->distance_mm <= 0 || pair->automatic_distance_mm <= 0 ||
            pair->single_direction || pair->direction_disagreement_mm > NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM) {
            continue;
        }
        const int64_t value = (int64_t)pair->automatic_distance_mm - pair->distance_mm;
        size_t position = count;
        while (position > 0 && offsets[position - 1] > value) {
            offsets[position] = offsets[position - 1];
            --position;
        }
        offsets[position] = value;
        ++count;
    }
    nrf_geometry_range_offset_t result = {0};
    result.count = count;
    if (count > 0) {
        result.span_mm = offsets[count - 1] - offsets[0];
        result.offset_mm = (int32_t)(count % 2 ? offsets[count / 2] :
                                     (offsets[count / 2 - 1] + offsets[count / 2]) / 2);
    }
    *out = result;
    return true;
}

static const nrf_geometry_node_input_t *find_node(const nrf_geometry_node_input_t *nodes,
                                                   size_t count, nrf_cs_node_id_t id)
{
    for (size_t i = 0; i < count; ++i) {
        if (nodes[i].node_id == id) {
            return &nodes[i];
        }
    }
    return NULL;
}

bool nrf_geometry_range_offset_apply(const nrf_cs_pair_result_t *pairs, size_t pair_count,
                                     const nrf_geometry_node_input_t *nodes, size_t node_count,
                                     int32_t offset_mm, nrf_cs_pair_result_t *out)
{
    if (pairs == NULL || nodes == NULL || out == NULL || pair_count == 0 ||
        pair_count > NRF_CS_MAX_PAIRS || node_count == 0 || node_count > NRF_CS_MAX_NODES) {
        return false;
    }
    const uintptr_t input_address = (uintptr_t)pairs;
    const uintptr_t output_address = (uintptr_t)out;
    const size_t bytes = pair_count * sizeof(*pairs);
    if ((input_address <= output_address && output_address - input_address < bytes) ||
        (output_address < input_address && input_address - output_address < bytes)) {
        return false;
    }
    memcpy(out, pairs, bytes);
    for (size_t i = 0; i < pair_count; ++i) {
        nrf_cs_pair_result_t *pair = &out[i];
        if (pair->state != NRF_CS_PAIR_VALID ||
            (pair->scheduler_quality_flags & (NRF_CS_QUALITY_MANUAL | NRF_CS_QUALITY_DEGRADED)) ||
            !single_ifft(pair)) {
            continue;
        }
        const nrf_geometry_node_input_t *a = find_node(nodes, node_count, pair->node_a);
        const nrf_geometry_node_input_t *b = find_node(nodes, node_count, pair->node_b);
        const int64_t corrected = (int64_t)pair->distance_mm - offset_mm;
        int64_t dz = a != NULL && b != NULL ? (int64_t)a->z_mm - b->z_mm : 0;
        if (dz < 0) {
            dz = -dz;
        }
        if (a == NULL || b == NULL || corrected <= 0 || corrected > INT32_MAX || corrected < dz) {
            pair->distance_mm = 0;
            pair->state = NRF_CS_PAIR_RETRY_EXHAUSTED;
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_OUTLIER;
            pair->last_failure = NRF_CS_FAILURE_OUTLIER;
        } else {
            pair->distance_mm = (int32_t)corrected;
        }
    }
    return true;
}

static int32_t degraded_candidate_distance(const nrf_cs_pair_result_t *pair)
{
    if (pair->state != NRF_CS_PAIR_RETRY_EXHAUSTED || (pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) ||
        pair->diag_samples[0] < NRF_GEOMETRY_DEGRADED_MIN_SAMPLES ||
        pair->diag_samples[1] < NRF_GEOMETRY_DEGRADED_MIN_SAMPLES ||
        pair->diag_distance_mm[0] <= 0 || pair->diag_distance_mm[1] <= 0) {
        return 0;
    }
    const int64_t first = pair->diag_distance_mm[0];
    const int64_t second = pair->diag_distance_mm[1];
    const int64_t mean = (first + second) / 2;
    const int64_t difference = first > second ? first - second : second - first;
    int64_t allowed = mean * NRF_GEOMETRY_DEGRADED_AGREE_PERCENT / 100;
    if (allowed < NRF_GEOMETRY_DEGRADED_AGREE_MM) {
        allowed = NRF_GEOMETRY_DEGRADED_AGREE_MM;
    }
    if (difference > allowed || mean > INT32_MAX) {
        return 0;
    }
    return (int32_t)mean;
}

/* Three distinct pairs that touch exactly three nodes are the three sides of a triangle. */
static bool degraded_is_triangle(const nrf_cs_pair_result_t *a, const nrf_cs_pair_result_t *b,
                                 const nrf_cs_pair_result_t *c)
{
    nrf_cs_node_id_t nodes[3];
    size_t count = 0;
    const nrf_cs_node_id_t ends[6] = {a->node_a, a->node_b, b->node_a, b->node_b, c->node_a, c->node_b};
    for (size_t i = 0; i < 6; ++i) {
        bool known = false;
        for (size_t n = 0; n < count; ++n) {
            known = known || nodes[n] == ends[i];
        }
        if (!known) {
            if (count == 3) {
                return false;
            }
            nodes[count++] = ends[i];
        }
    }
    return count == 3;
}

size_t nrf_geometry_degraded_pairs_promote(nrf_cs_pair_result_t *pairs, size_t pair_count)
{
    if (pairs == NULL || pair_count == 0 || pair_count > NRF_CS_MAX_PAIRS) {
        return 0;
    }
    int32_t candidate[NRF_CS_MAX_PAIRS];
    int32_t distance[NRF_CS_MAX_PAIRS];
    bool rejected[NRF_CS_MAX_PAIRS];
    for (size_t i = 0; i < pair_count; ++i) {
        candidate[i] = degraded_candidate_distance(&pairs[i]);
        distance[i] = pairs[i].state == NRF_CS_PAIR_VALID && pairs[i].distance_mm > 0 ? pairs[i].distance_mm
                                                                                      : candidate[i];
        rejected[i] = false;
    }
    const int64_t slack = NRF_GEOMETRY_DEGRADED_TRIANGLE_SLACK_MM;
    for (size_t i = 0; i < pair_count; ++i) {
        for (size_t j = i + 1; j < pair_count; ++j) {
            for (size_t k = j + 1; k < pair_count; ++k) {
                if (distance[i] <= 0 || distance[j] <= 0 || distance[k] <= 0 ||
                    !degraded_is_triangle(&pairs[i], &pairs[j], &pairs[k])) {
                    continue;
                }
                const int64_t a = distance[i];
                const int64_t b = distance[j];
                const int64_t c = distance[k];
                if (a > b + c + slack || b > a + c + slack || c > a + b + slack) {
                    rejected[i] = rejected[i] || candidate[i] > 0;
                    rejected[j] = rejected[j] || candidate[j] > 0;
                    rejected[k] = rejected[k] || candidate[k] > 0;
                }
            }
        }
    }
    size_t promoted = 0;
    for (size_t i = 0; i < pair_count; ++i) {
        if (candidate[i] <= 0 || rejected[i]) {
            continue;
        }
        nrf_cs_pair_result_t *pair = &pairs[i];
        const int32_t difference = pair->diag_distance_mm[0] > pair->diag_distance_mm[1]
            ? pair->diag_distance_mm[0] - pair->diag_distance_mm[1]
            : pair->diag_distance_mm[1] - pair->diag_distance_mm[0];
        pair->state = NRF_CS_PAIR_VALID;
        pair->last_failure = NRF_CS_FAILURE_NONE;
        pair->distance_mm = candidate[i];
        pair->automatic_distance_mm = 0;
        pair->confidence = NRF_GEOMETRY_DEGRADED_CONFIDENCE;
        pair->single_direction = false;
        pair->direction_disagreement_mm = difference;
        pair->spread_mm = difference;
        pair->valid_samples = (uint8_t)(pair->diag_samples[0] + pair->diag_samples[1]);
        pair->scheduler_quality_flags |= (uint16_t)(NRF_CS_QUALITY_DEGRADED | NRF_CS_QUALITY_LOW_CONFIDENCE);
        ++promoted;
    }
    return promoted;
}
