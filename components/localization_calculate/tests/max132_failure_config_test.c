#define main max132_unused_web_api_control_http_test_main
#include "../../../tests/host/web_api_control_http_test.c"
#undef main

#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MAX132_CONFIG_MKDIR(path) _mkdir(path)
#define MAX132_CONFIG_GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MAX132_CONFIG_MKDIR(path) mkdir(path, 0775)
#define MAX132_CONFIG_GETPID() getpid()
#endif

void max132_config_store_seed_saved_result(void);
void max132_config_store_assert_prior_result(void);

static void assert_missing_config_rejected(const char *body)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_calculate_calls == 0U);

    cJSON *decoded = cJSON_Parse(response);
    assert(decoded != NULL);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(decoded, "error");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(decoded, "reason");
    assert(cJSON_IsString(error) && strcmp(error->valuestring, "ESP_ERR_INVALID_ARG") == 0);
    assert(reason == NULL);
    cJSON_Delete(decoded);
    max132_config_store_assert_prior_result();
}

int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);
    char directory[80];
    const int length = snprintf(directory, sizeof(directory), "max132_config_%lu",
        (unsigned long)MAX132_CONFIG_GETPID());
    assert(length > 0 && length < (int)sizeof(directory));
    assert(MAX132_CONFIG_MKDIR(directory) == 0 || errno == EEXIST);
#ifdef _WIN32
    assert(_chdir(directory) == 0);
#else
    assert(chdir(directory) == 0);
#endif
    max132_config_store_seed_saved_result();

    /* KNOWN_DEFECT, build/max132_survey.md:111: invalid/missing calculation config gets only
     * an HTTP error and never persists a run-level reason. Minimal repro:
     * preserve a saved session result, omit noise_window_samples, submit the
     * real calculate_source API action, then list the session again. */
    static const char *const invalid_config_bodies[] = {
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S600001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25}",
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S600001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,\"noise_window_samples\":\"bad\",\"sta_window_samples\":5,"
        "\"lta_window_samples\":50,\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,"
        "\"refine_lookahead_samples\":10,\"refine_amplitude_fraction\":0.5,"
        "\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,\"semblance_window_half_span_samples\":25}"
    };
    for (size_t i = 0U; i < sizeof(invalid_config_bodies) / sizeof(invalid_config_bodies[0]); ++i) {
        assert_missing_config_rejected(invalid_config_bodies[i]);
    }

    puts("KNOWN_DEFECT build/max132_survey.md:111: invalid config returns an API error but persists no run reason; prior result remains readable");
    return 0;
}
