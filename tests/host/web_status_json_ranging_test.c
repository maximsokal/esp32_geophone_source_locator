#include <assert.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "nrf_topology_service.h"

static nrf_topology_ranging_progress_t s_progress;
static nrf_topology_ranging_result_t s_result;
static esp_err_t s_progress_error;
static esp_err_t s_result_error;

bool web_status_json_add_ranging(cJSON *root);

esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress)
{
    if (progress == NULL) return ESP_ERR_INVALID_ARG;
    if (s_progress_error == ESP_OK) *progress = s_progress;
    return s_progress_error;
}

esp_err_t nrf_topology_service_get_frozen_ranging_result(
    nrf_topology_ranging_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    if (s_result_error == ESP_OK) *result = s_result;
    return s_result_error;
}

const char *test_esp_err_to_name(int error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        default: return "ESP_ERR_OTHER";
    }
}

static const cJSON *item(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(value != NULL);
    return value;
}

static int number(const cJSON *object, const char *name)
{
    const cJSON *value = item(object, name);
    assert(cJSON_IsNumber(value));
    return value->valueint;
}

static void test_progress_and_frozen_result_json(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.active = true;
    s_progress.pairs_done = 2U;
    s_progress.pairs_total = 12U;
    s_progress.pair_count = 1U;
    s_progress.topology_generation = 21U;
    s_progress.membership_generation = 22U;
    s_progress.solution_residual_mm = 44;
    s_progress.geometry.node_count = 2U;
    s_progress.geometry.nodes[0] = (nrf_topology_ranging_node_t) {
        .node_id = 0U, .solved = true, .uncertainty_mm = 0,
    };
    s_progress.geometry.nodes[1] = (nrf_topology_ranging_node_t) {
        .node_id = 1U, .solved = true, .x_mm = 1000, .uncertainty_mm = 333,
    };
    s_progress.pairs[0] = (nrf_cs_pair_result_t) {
        .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 1000, .spread_mm = 300, .confidence = 850U,
        .valid_samples = 5U, .attempts = 1U, .single_direction = true,
        .direction_disagreement_mm = 100,
        .scheduler_quality_flags = NRF_CS_QUALITY_MANUAL,
        .automatic_distance_mm = 6001,
    };
    s_progress_error = ESP_OK;

    memset(&s_result, 0, sizeof(s_result));
    s_result.available = true;
    s_result.invalidated = true;
    s_result.pair_count = 1U;
    s_result.pairs[0] = s_progress.pairs[0];
    s_result.geometry = s_progress.geometry;
    s_result.topology_generation = 21U;
    s_result.membership_generation = 22U;
    s_result.timestamp_ms = 9000U;
    s_result_error = ESP_OK;

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_ranging(root));
    const cJSON *ranging = item(root, "ranging");
    assert(cJSON_IsTrue(item(ranging, "available")));
    const cJSON *progress = item(ranging, "progress");
    assert(cJSON_IsTrue(item(progress, "active")));
    assert(number(progress, "pairs_done") == 2);
    assert(number(progress, "pairs_total") == 12);
    assert(number(progress, "topology_generation") == 21);
    const cJSON *pairs = item(progress, "pairs");
    assert(cJSON_GetArraySize(pairs) == 1);
    const cJSON *pair = cJSON_GetArrayItem(pairs, 0);
    assert(strcmp(item(pair, "state")->valuestring, "valid") == 0);
    assert(cJSON_IsTrue(item(pair, "single_direction")));
    assert(number(pair, "direction_disagreement_mm") == 100);
    assert(cJSON_IsTrue(item(pair, "manual")));
    assert(number(pair, "automatic_distance_mm") == 6001);
    const cJSON *geometry = item(progress, "geometry");
    const cJSON *nodes = item(geometry, "nodes");
    assert(cJSON_GetArraySize(nodes) == 2);
    assert(number(cJSON_GetArrayItem(nodes, 1), "uncertainty_mm") == 333);
    const cJSON *last_result = item(ranging, "last_result");
    assert(cJSON_IsTrue(item(last_result, "available")));
    assert(cJSON_IsTrue(item(last_result, "invalidated")));
    assert(number(last_result, "membership_generation") == 22);
    assert(number(last_result, "timestamp_ms") == 9000);
    cJSON_Delete(root);
}

static void test_unavailable_progress_and_result_json(void)
{
    s_progress_error = ESP_ERR_INVALID_STATE;
    s_result_error = ESP_ERR_NOT_FOUND;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_ranging(root));
    const cJSON *ranging = item(root, "ranging");
    assert(!cJSON_IsTrue(item(ranging, "available")));
    assert(strcmp(item(ranging, "error")->valuestring, "ESP_ERR_INVALID_STATE") == 0);
    assert(!cJSON_IsTrue(item(item(ranging, "last_result"), "available")));
    cJSON_Delete(root);
    assert(!web_status_json_add_ranging(NULL));
}

int main(void)
{
    test_progress_and_frozen_result_json();
    test_unavailable_progress_and_result_json();
    return 0;
}
