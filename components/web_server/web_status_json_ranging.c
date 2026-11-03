#include "cJSON.h"
#include "esp_err.h"
#include "nrf_topology_service.h"
#include "ranging_radio_pause.h"

#include <stdbool.h>
#include <stddef.h>

static const char *pair_state_name(nrf_cs_pair_state_t state)
{
    switch (state) {
        case NRF_CS_PAIR_PENDING: return "pending";
        case NRF_CS_PAIR_VALID: return "valid";
        case NRF_CS_PAIR_MISSING: return "missing";
        case NRF_CS_PAIR_RETRY_EXHAUSTED: return "retry_exhausted";
        case NRF_CS_PAIR_CABLE_EXCEEDED: return "cable_exceeded";
        case NRF_CS_PAIR_DIRECTIONS_DISAGREE: return "directions_disagree";
        case NRF_CS_PAIR_UNKNOWN:
        default: return "unknown";
    }
}

static bool add_pairs(cJSON *object, const nrf_cs_pair_result_t *pairs, size_t count)
{
    if (count > NRF_CS_MAX_PAIRS) return false;
    cJSON *array = cJSON_AddArrayToObject(object, "pairs");
    if (array == NULL) return false;
    for (size_t i = 0U; i < count; ++i) {
        const nrf_cs_pair_result_t *pair = &pairs[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) return false;
        if (!cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            return false;
        }
        if (cJSON_AddNumberToObject(item, "node_a", pair->node_a) == NULL ||
            cJSON_AddNumberToObject(item, "node_b", pair->node_b) == NULL ||
            cJSON_AddStringToObject(item, "state", pair_state_name(pair->state)) == NULL ||
            cJSON_AddNumberToObject(item, "distance_mm", pair->distance_mm) == NULL ||
            cJSON_AddBoolToObject(item, "manual", (pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) != 0U) == NULL ||
            cJSON_AddNumberToObject(item, "automatic_distance_mm", pair->automatic_distance_mm) == NULL ||
            cJSON_AddNumberToObject(item, "spread_mm", pair->spread_mm) == NULL ||
            cJSON_AddNumberToObject(item, "confidence", pair->confidence) == NULL ||
            cJSON_AddNumberToObject(item, "valid_samples", pair->valid_samples) == NULL ||
            cJSON_AddNumberToObject(item, "attempts", pair->attempts) == NULL ||
            cJSON_AddBoolToObject(item, "single_direction", pair->single_direction) == NULL ||
            cJSON_AddNumberToObject(
                item,
                "direction_disagreement_mm",
                pair->direction_disagreement_mm
            ) == NULL) {
            return false;
        }
    }
    return true;
}

static bool add_geometry(
    cJSON *object,
    const nrf_topology_ranging_geometry_t *geometry,
    int32_t residual_mm)
{
    if (geometry->node_count > NRF_CS_MAX_NODES) return false;
    cJSON *section = cJSON_AddObjectToObject(object, "geometry");
    cJSON *nodes = section == NULL ? NULL : cJSON_AddArrayToObject(section, "nodes");
    if (section == NULL || nodes == NULL ||
        cJSON_AddNumberToObject(section, "residual_mm", residual_mm) == NULL ||
        cJSON_AddNumberToObject(section, "max_residual_mm", geometry->max_residual_mm) == NULL) {
        return false;
    }
    for (size_t i = 0U; i < geometry->node_count; ++i) {
        const nrf_topology_ranging_node_t *node = &geometry->nodes[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) return false;
        if (!cJSON_AddItemToArray(nodes, item)) {
            cJSON_Delete(item);
            return false;
        }
        if (cJSON_AddNumberToObject(item, "node_id", node->node_id) == NULL ||
            cJSON_AddBoolToObject(item, "solved", node->solved) == NULL ||
            cJSON_AddNumberToObject(item, "x_mm", node->x_mm) == NULL ||
            cJSON_AddNumberToObject(item, "y_mm", node->y_mm) == NULL ||
            cJSON_AddNumberToObject(item, "z_mm", node->z_mm) == NULL ||
            cJSON_AddNumberToObject(item, "node_residual_mm", node->residual_mm) == NULL ||
            cJSON_AddNumberToObject(item, "uncertainty_mm", node->uncertainty_mm) == NULL) {
            return false;
        }
    }
    return true;
}

static bool add_progress(cJSON *root, const nrf_topology_ranging_progress_t *progress)
{
    cJSON *object = cJSON_AddObjectToObject(root, "progress");
    if (object == NULL ||
        cJSON_AddBoolToObject(object, "active", progress->active) == NULL ||
        cJSON_AddBoolToObject(object, "complete", progress->complete) == NULL ||
        cJSON_AddBoolToObject(object, "success", progress->success) == NULL ||
        cJSON_AddBoolToObject(object, "partial", progress->partial) == NULL ||
        cJSON_AddNumberToObject(object, "pairs_done", (double)progress->pairs_done) == NULL ||
        cJSON_AddNumberToObject(object, "pairs_total", (double)progress->pairs_total) == NULL ||
        cJSON_AddNumberToObject(object, "topology_generation", progress->topology_generation) == NULL ||
        cJSON_AddNumberToObject(object, "membership_generation", progress->membership_generation) == NULL ||
        cJSON_AddNumberToObject(
            object,
            "main_height_reference_generation",
            progress->main_height_reference_generation
        ) == NULL ||
        cJSON_AddNumberToObject(object, "started_at_ms", (double)progress->started_at_ms) == NULL ||
        cJSON_AddNumberToObject(object, "updated_at_ms", (double)progress->updated_at_ms) == NULL ||
        cJSON_AddStringToObject(object, "failure_reason", progress->failure_reason) == NULL ||
        cJSON_AddNumberToObject(object, "solution_residual_mm", progress->solution_residual_mm) == NULL ||
        !add_pairs(object, progress->pairs, progress->pair_count) ||
        !add_geometry(object, &progress->geometry, progress->solution_residual_mm)) {
        return false;
    }
    return true;
}

static bool add_result(cJSON *root, const nrf_topology_ranging_result_t *result)
{
    cJSON *object = cJSON_AddObjectToObject(root, "last_result");
    if (object == NULL ||
        cJSON_AddBoolToObject(object, "available", result->available) == NULL ||
        cJSON_AddBoolToObject(object, "invalidated", result->invalidated) == NULL ||
        cJSON_AddBoolToObject(object, "success", result->success) == NULL) {
        return false;
    }
    if (!result->available) return true;
    if (cJSON_AddNumberToObject(object, "topology_generation", result->topology_generation) == NULL ||
        cJSON_AddNumberToObject(object, "membership_generation", result->membership_generation) == NULL ||
        cJSON_AddNumberToObject(
            object,
            "main_height_reference_generation",
            result->main_height_reference_generation
        ) == NULL ||
        cJSON_AddNumberToObject(object, "timestamp_ms", (double)result->timestamp_ms) == NULL ||
        !add_pairs(object, result->pairs, result->pair_count) ||
        !add_geometry(object, &result->geometry, result->geometry.max_residual_mm)) {
        return false;
    }
    return true;
}

bool web_status_json_add_ranging(cJSON *root)
{
    if (root == NULL) return false;
    cJSON *object = cJSON_AddObjectToObject(root, "ranging");
    if (object == NULL) return false;

    nrf_topology_ranging_progress_t progress = {0};
    const esp_err_t progress_error = nrf_topology_service_ranging_get_progress(&progress);
    if (cJSON_AddBoolToObject(object, "available", progress_error == ESP_OK) == NULL) {
        return false;
    }
    /*
     * The Wi-Fi radio is off while the nodes range. "pending" is the notice period (the page can still say that it is about to
     * go off), "off" is only ever seen by a client that is still connected another way, "resuming" is the radio coming back.
     */
    if (cJSON_AddStringToObject(object, "radio", ranging_radio_state_name(ranging_radio_pause_state())) == NULL ||
        cJSON_AddBoolToObject(object, "radio_stop_failed", ranging_radio_pause_stop_failed()) == NULL ||
        cJSON_AddBoolToObject(object, "radio_resume_failed", ranging_radio_pause_resume_failed()) == NULL) {
        return false;
    }
    if (progress_error == ESP_OK) {
        if (!add_progress(object, &progress)) return false;
    } else if (cJSON_AddStringToObject(object, "error", esp_err_to_name(progress_error)) == NULL) {
        return false;
    }

    nrf_topology_ranging_result_t result = {0};
    if (nrf_topology_service_get_frozen_ranging_result(&result) != ESP_OK) {
        result.available = false;
    }
    return add_result(object, &result);
}
