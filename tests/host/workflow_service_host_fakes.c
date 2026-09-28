#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object_store.h"
#include "workflow_service_host_fakes.h"

unsigned workflow_test_scan_calls;
bool workflow_test_scan_present;
esp_err_t workflow_test_settings_error;
const char *workflow_test_settings_json;
esp_err_t workflow_test_settings_save_result;
unsigned workflow_test_settings_save_calls;
char workflow_test_saved_settings[1024];
char workflow_test_saved_object_id[16];
bool workflow_test_saved_settings_valid;
localization_job_status_t workflow_test_job_status;
esp_err_t workflow_test_job_submit_result;
unsigned workflow_test_job_submit_calls;
uint32_t workflow_test_next_job_id;
localization_calculate_config_t workflow_test_submitted_config;
uint32_t workflow_test_submitted_budget_ms;
char workflow_test_submitted_object_id[16];
char workflow_test_submitted_session_id[16];

static const char s_valid_settings[] =
    "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":5,"
    "\"propagation_model_name\":\"tap_calibrated\",\"timing_uncertainty_us\":2000,"
    "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
    "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,"
    "\"refine_lookahead_samples\":10,\"refine_amplitude_fraction\":0.5,"
    "\"snr_signal_window_samples\":50,\"minimum_snr\":2,"
    "\"semblance_window_half_span_samples\":25}";

void workflow_service_host_fakes_reset(void)
{
    workflow_test_scan_calls = 0U;
    workflow_test_scan_present = false;
    workflow_test_settings_error = ESP_OK;
    workflow_test_settings_json = s_valid_settings;
    workflow_test_settings_save_result = ESP_OK;
    workflow_test_settings_save_calls = 0U;
    workflow_test_saved_settings[0] = '\0';
    workflow_test_saved_object_id[0] = '\0';
    workflow_test_saved_settings_valid = false;
    memset(&workflow_test_job_status, 0, sizeof(workflow_test_job_status));
    workflow_test_job_status.state = LOCALIZATION_JOB_IDLE;
    workflow_test_job_submit_result = ESP_OK;
    workflow_test_job_submit_calls = 0U;
    workflow_test_next_job_id = 100U;
    memset(&workflow_test_submitted_config, 0, sizeof(workflow_test_submitted_config));
    workflow_test_submitted_budget_ms = 0U;
    workflow_test_submitted_object_id[0] = '\0';
    workflow_test_submitted_session_id[0] = '\0';
}

esp_err_t object_store_scan_present(const char *object_id, const char *session_id, bool *present_out)
{
    assert(object_id != NULL && present_out != NULL);
    (void)session_id;
    ++workflow_test_scan_calls;
    *present_out = workflow_test_scan_present;
    return ESP_OK;
}

esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out)
{
    assert(object_id != NULL && json_out != NULL);
    *json_out = NULL;
    if (workflow_test_settings_error != ESP_OK) return workflow_test_settings_error;
    const char *settings = workflow_test_saved_settings_valid
        ? workflow_test_saved_settings : workflow_test_settings_json;
    if (settings == NULL) return ESP_ERR_NOT_FOUND;
    const size_t size = strlen(settings) + 1U;
    *json_out = malloc(size);
    if (*json_out == NULL) return ESP_ERR_NO_MEM;
    memcpy(*json_out, settings, size);
    return ESP_OK;
}

esp_err_t object_store_set_calc_settings_json(const char *object_id, const char *json)
{
    assert(object_id != NULL && json != NULL);
    ++workflow_test_settings_save_calls;
    if (workflow_test_settings_save_result != ESP_OK) return workflow_test_settings_save_result;
    if (strlen(json) >= sizeof(workflow_test_saved_settings)) return ESP_ERR_INVALID_ARG;
    (void)snprintf(workflow_test_saved_object_id, sizeof(workflow_test_saved_object_id), "%s", object_id);
    (void)snprintf(workflow_test_saved_settings, sizeof(workflow_test_saved_settings), "%s", json);
    workflow_test_saved_settings_valid = true;
    return ESP_OK;
}

esp_err_t localization_job_init(void) { return ESP_OK; }

esp_err_t localization_job_submit(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint32_t *job_id_out)
{
    assert(object_id != NULL && session_id != NULL && config != NULL);
    ++workflow_test_job_submit_calls;
    if (workflow_test_job_submit_result != ESP_OK) return workflow_test_job_submit_result;
    uint32_t id = ++workflow_test_next_job_id;
    if (id == 0U) id = ++workflow_test_next_job_id;
    workflow_test_job_status = (localization_job_status_t){
        .state = LOCALIZATION_JOB_QUEUED,
        .job_id = id,
    };
    (void)snprintf(workflow_test_job_status.object_id,
        sizeof(workflow_test_job_status.object_id), "%s", object_id);
    (void)snprintf(workflow_test_job_status.session_id,
        sizeof(workflow_test_job_status.session_id), "%s", session_id);
    workflow_test_submitted_config = *config;
    workflow_test_submitted_budget_ms = budget_ms;
    (void)snprintf(workflow_test_submitted_object_id,
        sizeof(workflow_test_submitted_object_id), "%s", object_id);
    (void)snprintf(workflow_test_submitted_session_id,
        sizeof(workflow_test_submitted_session_id), "%s", session_id);
    if (job_id_out != NULL) *job_id_out = id;
    return ESP_OK;
}

void localization_job_get_status(localization_job_status_t *status_out)
{
    assert(status_out != NULL);
    *status_out = workflow_test_job_status;
}
