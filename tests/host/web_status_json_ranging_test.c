#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "ranging_radio_pause.h"
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
    s_progress.geometry.range_offset_mm = -150;
    s_progress.geometry.range_offset_manual = true;
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
    assert(!cJSON_IsTrue(item(pair, "degraded")));
    assert(number(pair, "automatic_distance_mm") == 6001);
    const cJSON *geometry = item(progress, "geometry");
    assert(number(geometry, "range_offset_mm") == -150);
    assert(strcmp(item(geometry, "range_offset_source")->valuestring, "manual") == 0);
    const cJSON *nodes = item(geometry, "nodes");
    assert(cJSON_GetArraySize(nodes) == 2);
    assert(number(cJSON_GetArrayItem(nodes, 1), "uncertainty_mm") == 333);
    const cJSON *last_result = item(ranging, "last_result");
    assert(cJSON_IsTrue(item(last_result, "available")));
    assert(cJSON_IsTrue(item(last_result, "invalidated")));
    assert(number(last_result, "membership_generation") == 22);
    assert(number(last_result, "timestamp_ms") == 9000);
    assert(number(item(last_result, "geometry"), "range_offset_mm") == -150);
    assert(strcmp(item(item(last_result, "geometry"), "range_offset_source")->valuestring, "manual") == 0);
    cJSON_Delete(root);

    s_progress.geometry.range_offset_mm = 0;
    s_progress.geometry.range_offset_manual = false;
    s_result.geometry = s_progress.geometry;
    root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    ranging = item(root, "ranging");
    geometry = item(item(ranging, "progress"), "geometry");
    assert(number(geometry, "range_offset_mm") == 0);
    assert(strcmp(item(geometry, "range_offset_source")->valuestring, "none") == 0);
    geometry = item(item(ranging, "last_result"), "geometry");
    assert(number(geometry, "range_offset_mm") == 0);
    assert(strcmp(item(geometry, "range_offset_source")->valuestring, "none") == 0);
    cJSON_Delete(root);

    /* A pair accepted from low-confidence samples is reported as degraded, and is not a manual one. */
    s_progress.pairs[0].scheduler_quality_flags = NRF_CS_QUALITY_DEGRADED | NRF_CS_QUALITY_LOW_CONFIDENCE;
    root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    pair = cJSON_GetArrayItem(item(item(item(root, "ranging"), "progress"), "pairs"), 0);
    assert(cJSON_IsTrue(item(pair, "degraded")));
    assert(!cJSON_IsTrue(item(pair, "manual")));
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

static void test_usable_partial_geometry_preserves_success_and_pair_failures(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.complete = true;
    s_progress.success = true;
    s_progress.partial = true;
    s_progress.pair_count = 2U;
    s_progress.pairs[0] = (nrf_cs_pair_result_t){.node_a=0U,.node_b=1U,.state=NRF_CS_PAIR_VALID,
        .distance_mm=2300,.automatic_distance_mm=6000,.scheduler_quality_flags=NRF_CS_QUALITY_MANUAL};
    s_progress.pairs[1] = (nrf_cs_pair_result_t){.node_a=3U,.node_b=4U,.state=NRF_CS_PAIR_RETRY_EXHAUSTED};
    s_progress_error = ESP_OK;
    s_result_error = ESP_ERR_NOT_FOUND;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *progress = item(item(root, "ranging"), "progress");
    assert(cJSON_IsTrue(item(progress, "success")) && cJSON_IsTrue(item(progress, "partial")));
    const cJSON *pairs = item(progress, "pairs");
    assert(cJSON_IsTrue(item(cJSON_GetArrayItem(pairs, 0), "manual")));
    assert(number(cJSON_GetArrayItem(pairs, 0), "automatic_distance_mm") == 6000);
    assert(strcmp(item(cJSON_GetArrayItem(pairs, 1), "state")->valuestring, "retry_exhausted") == 0);
    cJSON_Delete(root);
}

/* Bench 2026-11-03: a pair without a distance still reports what each direction measured, and why it has no distance. */
static void test_a_failed_pair_reports_the_numbers_of_both_directions_and_its_cause(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.pair_count = 2U;
    s_progress.pairs[0] = (nrf_cs_pair_result_t) {
        .node_a = 0U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 3903, .valid_samples = 18U,
        .diag_distance_mm = { 3918, 3889 }, .diag_samples = { 9U, 9U }, .diag_accepted = { 9U, 9U },
    };
    s_progress.pairs[1] = (nrf_cs_pair_result_t) {
        .node_a = 0U, .node_b = 4U, .state = NRF_CS_PAIR_RETRY_EXHAUSTED, .attempts = 13U,
        .diag_distance_mm = { 8845, 6421 }, .diag_samples = { 3U, 10U }, .diag_accepted = { 1U, 9U },
    };
    s_progress_error = ESP_OK;
    s_result_error = ESP_ERR_NOT_FOUND;

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *pairs = item(item(item(root, "ranging"), "progress"), "pairs");
    const cJSON *valid = cJSON_GetArrayItem(pairs, 0);
    const cJSON *failed = cJSON_GetArrayItem(pairs, 1);
    assert(strcmp(item(valid, "cause")->valuestring, "none") == 0 && number(valid, "diag_ab_mm") == 3918);
    assert(strcmp(item(failed, "state")->valuestring, "retry_exhausted") == 0 && number(failed, "distance_mm") == 0);
    assert(strcmp(item(failed, "cause")->valuestring, "low_confidence") == 0);
    assert(number(failed, "diag_ab_mm") == 8845 && number(failed, "diag_ba_mm") == 6421);
    assert(number(failed, "diag_ab_samples") == 3 && number(failed, "diag_ab_accepted") == 1);
    assert(number(failed, "diag_ba_samples") == 10 && number(failed, "diag_ba_accepted") == 9);
    cJSON_Delete(root);
}

/* The Wi-Fi radio is off while the nodes range: the page is told what is going on, also while no progress exists yet. */
static void test_the_radio_state_is_reported_in_every_phase(void)
{
    static const struct { ranging_radio_state_t state; const char *name; } phases[] = {
        { RANGING_RADIO_IDLE, "idle" }, { RANGING_RADIO_PENDING, "pending" }, { RANGING_RADIO_OFF, "off" },
        { RANGING_RADIO_RESUMING, "resuming" },
    };
    for (size_t i = 0U; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        for (int available = 0; available < 2; ++available) {
            g_radio_pause_state = phases[i].state;
            s_progress_error = available != 0 ? ESP_OK : ESP_ERR_INVALID_STATE;     /* no progress while the page waits for the radio */
            cJSON *root = cJSON_CreateObject();
            assert(root != NULL && web_status_json_add_ranging(root));
            const cJSON *ranging = item(root, "ranging");
            assert(strcmp(item(ranging, "radio")->valuestring, phases[i].name) == 0);
            assert(!cJSON_IsTrue(item(ranging, "radio_stop_failed")) && !cJSON_IsTrue(item(ranging, "radio_resume_failed")));
            cJSON_Delete(root);
        }
    }
    g_radio_pause_state = RANGING_RADIO_RESUMING;
    g_radio_pause_stop_failed = true;
    g_radio_pause_resume_failed = true;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    assert(cJSON_IsTrue(item(item(root, "ranging"), "radio_stop_failed")));
    assert(cJSON_IsTrue(item(item(root, "ranging"), "radio_resume_failed")));
    cJSON_Delete(root);
    g_radio_pause_state = RANGING_RADIO_IDLE;
    g_radio_pause_stop_failed = false;
    g_radio_pause_resume_failed = false;
}

/* ---- REMOTE_ONLY (no nRF MAIN): the status says which topology and which node is the origin ---- */

static void fill_remote_only_geometry(nrf_topology_ranging_geometry_t *geometry)
{
    memset(geometry, 0, sizeof(*geometry));
    geometry->node_count = 4U;
    for (unsigned i = 0U; i < 4U; ++i) {
        geometry->nodes[i] = (nrf_topology_ranging_node_t){
            .node_id = (nrf_cs_node_id_t)(i + 1U), .solved = true, .x_mm = (int32_t)(100 * i), .uncertainty_mm = i == 0U ? 0 : 150,
        };
    }
}

static void remote_only_json_has_mode_and_anchor(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.complete = true;
    s_progress.success = true;
    s_progress.topology_mode = NRF_TOPOLOGY_MODE_REMOTE_ONLY;
    s_progress.anchor_node_id = 1U;
    fill_remote_only_geometry(&s_progress.geometry);
    s_progress_error = ESP_OK;
    memset(&s_result, 0, sizeof(s_result));
    s_result.available = true;
    s_result.success = true;
    s_result.topology_mode = NRF_TOPOLOGY_MODE_REMOTE_ONLY;
    s_result.anchor_node_id = 1U;
    fill_remote_only_geometry(&s_result.geometry);
    s_result_error = ESP_OK;

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *ranging = item(root, "ranging");
    const cJSON *progress = item(ranging, "progress");
    const cJSON *last = item(ranging, "last_result");
    assert(strcmp(item(progress, "topology_mode")->valuestring, "REMOTE_ONLY") == 0);
    assert(number(progress, "anchor_node_id") == 1);
    assert(strcmp(item(last, "topology_mode")->valuestring, "REMOTE_ONLY") == 0);
    assert(number(last, "anchor_node_id") == 1);
    const cJSON *nodes = item(item(last, "geometry"), "nodes");
    assert(cJSON_GetArraySize(nodes) == 4);
    for (int i = 0; i < 4; ++i) assert(number(cJSON_GetArrayItem(nodes, i), "node_id") == i + 1);
    cJSON_Delete(root);
}

static void legacy_result_without_mode_is_reported_as_unrecorded(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress_error = ESP_OK;
    memset(&s_result, 0, sizeof(s_result));
    s_result.available = true;
    s_result.geometry.node_count = 1U;
    s_result_error = ESP_OK;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *last = item(item(root, "ranging"), "last_result");
    /* Not guessed as MAIN_PRESENT here: the consumer reads the old scheme itself and the anchor is not invented. */
    assert(strcmp(item(last, "topology_mode")->valuestring, "UNRECORDED") == 0);
    assert(cJSON_IsNull(item(last, "anchor_node_id")));
    cJSON_Delete(root);
}

static void serialize_missing_pairs_not_zero(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.complete = true;
    s_progress.topology_mode = NRF_TOPOLOGY_MODE_REMOTE_ONLY;
    s_progress.anchor_node_id = 1U;
    s_progress.pair_count = 3U;
    s_progress.pairs[0] = (nrf_cs_pair_result_t){.node_a = 1U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 1000};
    s_progress.pairs[1] = (nrf_cs_pair_result_t){.node_a = 1U, .node_b = 3U, .state = NRF_CS_PAIR_MISSING};
    s_progress.pairs[2] = (nrf_cs_pair_result_t){.node_a = 2U, .node_b = 3U, .state = NRF_CS_PAIR_RETRY_EXHAUSTED};
    s_progress_error = ESP_OK;
    s_result_error = ESP_ERR_NOT_FOUND;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *pairs = item(item(item(root, "ranging"), "progress"), "pairs");
    assert(cJSON_IsTrue(item(cJSON_GetArrayItem(pairs, 0), "distance_valid")));
    /* A pair that was not measured has no distance: it says so, the 0 mm it carries is not a reading. */
    for (int i = 1; i <= 2; ++i) {
        const cJSON *pair = cJSON_GetArrayItem(pairs, i);
        assert(!cJSON_IsTrue(item(pair, "distance_valid")));
        assert(strcmp(item(pair, "state")->valuestring, i == 1 ? "missing" : "retry_exhausted") == 0);
    }
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } named[] = {
        {"remote_only_json_has_mode_and_anchor", remote_only_json_has_mode_and_anchor},
        {"legacy_result_without_mode_is_reported_as_unrecorded", legacy_result_without_mode_is_reported_as_unrecorded},
        {"serialize_missing_pairs_not_zero", serialize_missing_pairs_not_zero},
    };
    if (argc > 1) { /* one argument runs only the named REMOTE_ONLY case (to show a single RED/GREEN case) */
        for (size_t i = 0U; i < sizeof(named) / sizeof(named[0]); ++i) {
            if (strcmp(argv[1], named[i].name) == 0) { named[i].run(); return 0; }
        }
        return 2;
    }
    remote_only_json_has_mode_and_anchor();
    legacy_result_without_mode_is_reported_as_unrecorded();
    serialize_missing_pairs_not_zero();
    test_progress_and_frozen_result_json();
    test_unavailable_progress_and_result_json();
    test_usable_partial_geometry_preserves_success_and_pair_failures();
    test_a_failed_pair_reports_the_numbers_of_both_directions_and_its_cause();
    test_the_radio_state_is_reported_in_every_phase();
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress_error = ESP_OK;
    s_progress.complete = true;
    s_progress.partial = true;
    strcpy(s_progress.failure_reason, "geometry_uncertain:1");
    s_progress.geometry.node_count = 1U;
    s_progress.geometry.nodes[0] = (nrf_topology_ranging_node_t){.node_id=1U,.solved=true,.uncertainty_mm=3137475};
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL && web_status_json_add_ranging(root));
    const cJSON *progress = item(item(root, "ranging"), "progress");
    assert(!cJSON_IsTrue(item(progress, "success")) && cJSON_IsTrue(item(progress, "partial")));
    assert(strcmp(item(progress, "failure_reason")->valuestring, "geometry_uncertain:1") == 0);
    assert(number(cJSON_GetArrayItem(item(item(progress, "geometry"), "nodes"), 0), "uncertainty_mm") == 3137475);
    cJSON_Delete(root);
    return 0;
}
