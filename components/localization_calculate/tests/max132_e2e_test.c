/*
 * MAX-132 SLICE 12, K1-K3: host-only synthetic end-to-end acceptance.
 *
 * This fixture is AD7606-like, not a physical recording. A host pass proves
 * this software chain only; it is NOT physical, target, or HIL evidence.
 *
 * The frozen JSON comparison normalizes volatile millisecond timestamps
 * (computed_at_ms, geometry_computed_at_ms, start_uptime_ms,
 * source_sample_uptime_ms, esp_receipt_uptime_ms, captured_at_ms) to 1 and
 * elapsed/computation-time fields to 1 if a later response adds them. All
 * machine results, picks, reasons, and positions remain compared.
 */

#ifdef _MSC_VER
#pragma warning(disable: 4005)
#endif

#define esp_timer_get_time max132_ad7606_unused_test_timer
#define main ad7606_driver_host_test_main
#include "../../ad7606_driver/tests/ad7606_driver_host_test.c"
#undef main
#undef esp_timer_get_time

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ad7606_driver.h"
#include "localization_job.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "localization_calculate.h"
#include "localization_calculate_policy.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "psram_model.h"
#include "scan_occupancy.h"
#include "seismic_buffer.h"
#include "seismic_dsp.h"
#include "seismic_recorder.h"
#include "seismic_recorder_geometry_policy.h"
#include "storage_manager.h"
#include "web_server_internal.h"

#include "esp_log.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include "../../ad7606_driver/ad7606_driver.c"
#include "../../ad7606_driver/ad7606_serial.c"
#undef ESP_LOGI
#undef ESP_LOGW

#ifndef CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
#error "The E2E fixture requires the configured production AD7606 sample rate"
#endif

#define E2E_SAMPLE_RATE_HZ CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
#define E2E_FRAME_COUNT ((size_t)E2E_SAMPLE_RATE_HZ)
#define E2E_OBJECT_ID "OBJ_0132"
#define E2E_SESSION_ID "S013212"
#define E2E_PLANAR_MATRIX_SESSION_ID "S013213"
#define E2E_REMOTE_ONLY_SESSION_ID "S013214"
#define E2E_REMOTE_ONLY_CONTROL_SESSION_ID "S013215"
#define E2E_REMOTE_ONLY_TOLERANCE_M 0.10
#define E2E_COMPUTED_AT_MS UINT64_C(132012)
#define E2E_P_SPEED_MPS 340.0
#define E2E_S_SPEED_MPS 190.0
#define E2E_EMISSION_TIME_S 0.15
#define E2E_PULSE_WIDTH_SAMPLES 80U
#define E2E_PULSE_FREQUENCY_HZ 900.0
#define E2E_PULSE_HALF_SUPPORT_S \
    ((double)E2E_PULSE_WIDTH_SAMPLES / (2.0 * (double)E2E_SAMPLE_RATE_HZ))
#define E2E_PICK_TOLERANCE_S \
    (E2E_PULSE_HALF_SUPPORT_S + 1.0 / (double)E2E_SAMPLE_RATE_HZ)
#define E2E_HTTP_RESPONSE_CAPACITY (256U * 1024U)

_Static_assert(AD7606_CHANNEL_COUNT == 4U, "fixture uses the production four-word AD7606 frame");
_Static_assert(AD7606_SIGNAL_CHANNEL_COUNT == 4U, "all production AD7606 signal words are used");
_Static_assert(SEISMIC_CHANNEL_COUNT == 4, "decoded frames use the production seismic channel layout");
_Static_assert(OBJECT_STORE_POD_COUNT == 4U, "fixture geometry has four production pod slots");

static const localization_point3d_t s_e2e_sensor_positions[AD7606_CHANNEL_COUNT] = {
    { 0.0, 0.0, 0.0 },
    { 2.0, 0.0, 0.0 },
    { 0.0, 2.0, 0.0 },
    { 0.0, 0.0, 2.0 },
};

static const localization_point3d_t s_e2e_source_position = { 0.70, 0.75, 0.80 };
/* The layout the fixture builder places the sensors on: the fixed one, or the scan-frame one of the REMOTE_ONLY pass. */
static const localization_point3d_t *s_e2e_layout = s_e2e_sensor_positions;

typedef struct {
    const char *name;
    bool solved_3_sensor_planar;
    bool solved_4_sensor_3d;
} e2e_method_expectation_t;

static const e2e_method_expectation_t s_e2e_method_expectations[] = {
    { "tdoa_gauss_newton", false, true },
    { "traveltime_inversion", true, true },
    { "semblance", true, true },
    { "energy_baseline", true, true },
    { "tdcas", false, false },
    { "psafuvs", false, false },
    { "suea", true, true },
    { "xcorr_tdoa", true, true },
    { "robust_tdoa", true, true },
    { "psp_joint", true, true },
    { "bayesian", true, true },
    { "eikonal_fmm", true, true },
};

static const char *const s_e2e_scan_methods[] = {
    "traveltime_scan_constrained",
    "semblance_scan_constrained",
    "tdcas_scan_constrained",
    "psafuvs_scan_constrained",
    "suea_scan_voids",
    "eikonal_scan_voids",
    "semblance_eikonal_voids",
    "bayesian_scan_voids",
};

static object_store_geometry_t s_e2e_geometry;
static seismic_recorder_session_summary_t s_e2e_session_summary;
static int16_t (*s_e2e_raw_frames)[AD7606_CHANNEL_COUNT];
static seismic_frame_t *s_e2e_decoded_frames;
static size_t s_e2e_frame_count;
static uint32_t s_e2e_rng_state = UINT32_C(0x13212E2E);
static int64_t s_e2e_now_us = INT64_C(1000000);
static char s_e2e_http_response[E2E_HTTP_RESPONSE_CAPACITY];

static void e2e_require(bool condition, const char *stage, const char *detail)
{
    if (!condition) {
        (void)fprintf(stderr, "[MAX-132 E2E FAIL] stage=%s detail=%s\n", stage, detail);
        (void)fflush(stderr);
        exit(EXIT_FAILURE);
    }
}

int64_t esp_timer_get_time(void)
{
    return s_e2e_now_us;
}

/* The web handlers this test includes also serve the phone's download and result upload: nothing here calls them. */
bool storage_bulk_admission_is_protected(void) { return false; }
esp_err_t storage_bulk_admission_run_chunk(esp_err_t (*fn)(void *), void *ctx) { return fn(ctx); }
void localization_job_get_status(localization_job_status_t *status_out) { memset(status_out, 0, sizeof(*status_out)); }

esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t read_count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool truncated = fgetc(file) != EOF;
    const int close_result = fclose(file);
    if (truncated || close_result != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[read_count] = '\0';
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool wrote = fputs(text, file) >= 0;
    return fclose(file) == 0 && wrote ? ESP_OK : ESP_FAIL;
}

esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    (void)object_id;
    (void)session_id;
    if (geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *geometry = s_e2e_geometry;
    return ESP_OK;
}

/*
 * REMOTE_ONLY pass: the layout Channel Sounding froze (in its own, arbitrary frame) and the AprilTag-centre positions the
 * scan registered. The real policy decides which one the localization gets, exactly as the firmware does.
 */
typedef enum {
    E2E_GEOMETRY_AS_FIXTURE = 0,
    E2E_GEOMETRY_CHANNEL_SOUNDING_ONLY,
    E2E_GEOMETRY_REGISTERED_OVER_CHANNEL_SOUNDING,
} e2e_geometry_mode_t;

static e2e_geometry_mode_t s_e2e_geometry_mode = E2E_GEOMETRY_AS_FIXTURE;
static object_store_geometry_t s_e2e_channel_sounding_layout;
static seismic_recorder_registered_positions_t s_e2e_registered_positions;

esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source,
    esp_err_t *registered_status
)
{
    if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    if (registered_status != NULL) *registered_status = ESP_OK;
    if (s_e2e_geometry_mode == E2E_GEOMETRY_AS_FIXTURE) {
        return seismic_recorder_read_session_geometry(object_id, session_id, geometry);
    }
    if (geometry == NULL) return ESP_ERR_INVALID_ARG;
    *geometry = s_e2e_channel_sounding_layout;
    if (s_e2e_geometry_mode == E2E_GEOMETRY_REGISTERED_OVER_CHANNEL_SOUNDING) {
        seismic_recorder_session_participation_t session;
        memset(&session, 0, sizeof(session));
        session.geometry_provenance.available = true;
        snprintf(session.geometry_provenance.topology_mode, sizeof(session.geometry_provenance.topology_mode), "remote_only");
        session.geometry_provenance.anchor_known = true;
        session.geometry_provenance.anchor_node_id = 1U;
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            session.pods[i] = (seismic_recorder_pod_participation_t){
                .assigned = true, .node_id_known = true, .node_id = (uint8_t)(i + 1U) };
        }
        const esp_err_t applied = seismic_recorder_geometry_apply_registered_positions(
            &session, &s_e2e_registered_positions, geometry);
        if (registered_status != NULL) *registered_status = applied;
        if (applied == ESP_OK && source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED;
    }
    return ESP_OK;
}

/* The tag layout the localization may read beside the geometry: none in this fixture. */
/* The recorder's registration scan binding (issue #31): this harness has no registration file. */
esp_err_t seismic_recorder_read_session_registration_scan_id(const char *object_id, const char *session_id, char scan_frame_id[SEISMIC_RECORDER_REGISTRATION_SCAN_ID_SIZE])
{
    (void)object_id;
    (void)session_id;
    if (scan_frame_id != NULL) scan_frame_id[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

esp_err_t seismic_recorder_read_session_tag_layout(const char *object_id, const char *session_id, seismic_recorder_tag_layout_t *layout)
{
    (void)object_id;
    (void)session_id;
    if (layout != NULL) memset(layout, 0, sizeof(*layout));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t seismic_recorder_get_session_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    if (summary_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *summary_out = s_e2e_session_summary;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_range_ex(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out,
    seismic_recorder_raw_window_stats_t *raw_stats_out
)
{
    (void)object_id;
    (void)session_id;
    if (frames == NULL || frame_count_out == NULL || start_index > s_e2e_frame_count || max_frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t remaining = s_e2e_frame_count - (size_t)start_index;
    const size_t count = remaining < max_frames ? remaining : max_frames;
    memcpy(frames, s_e2e_decoded_frames + (size_t)start_index, count * sizeof(*frames));
    for (size_t i = 0U; i < count; ++i) {
        const size_t sample = (size_t)start_index + i;
        if (source_sample_indices != NULL) source_sample_indices[i] = (uint64_t)sample;
        if (gap_before != NULL) gap_before[i] = false;
    }
    if (raw_stats_out != NULL) {
        memset(raw_stats_out, 0, sizeof(*raw_stats_out));
        raw_stats_out->samples_examined = count;
        for (size_t sample = (size_t)start_index; sample < (size_t)start_index + count; ++sample) {
            for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                if (s_e2e_raw_frames[sample][channel] == INT16_MIN ||
                    s_e2e_raw_frames[sample][channel] == INT16_MAX) {
                    ++raw_stats_out->rail_sample_counts[channel];
                }
            }
        }
    }
    *frame_count_out = count;
    if (summary_out != NULL) {
        *summary_out = s_e2e_session_summary;
    }
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_participation(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_participation_t *participation
)
{
    (void)object_id;
    (void)session_id;
    if (participation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(participation, 0, sizeof(*participation));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t seismic_recorder_read_session_cs_pairs(
    const char *object_id,
    const char *session_id,
    object_store_cs_pairs_t *cs_pairs
)
{
    (void)object_id;
    (void)session_id;
    if (cs_pairs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cs_pairs, 0, sizeof(*cs_pairs));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out)
{
    (void)object_id;
    if (json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t localization_job_status_json(char **json_out)
{
    static const char status_json[] = "{\"state\":\"idle\"}";
    if (json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = malloc(sizeof(status_json));
    if (*json_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(*json_out, status_json, sizeof(status_json));
    return ESP_OK;
}

static esp_err_t e2e_query_value(const char *query, const char *key, char *value, size_t value_size)
{
    if (query == NULL || key == NULL || value == NULL || value_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        const char *equals = strchr(cursor, '=');
        if (equals != NULL && (end == NULL || equals < end) && (size_t)(equals - cursor) == key_length &&
            strncmp(cursor, key, key_length) == 0) {
            const char *start = equals + 1;
            const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
            if (length == 0U || length >= value_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(value, start, length);
            value[length] = '\0';
            return ESP_OK;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request != NULL && request->query != NULL ? strlen(request->query) : 0U;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if (request == NULL || request->query == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(request->query);
    if (length >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, request->query, length + 1U);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t value_size)
{
    return e2e_query_value(query, key, value, value_size);
}

int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return ESP_FAIL;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    if (request == NULL || type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    request->content_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    if (request == NULL || field == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(field, "Cache-Control") == 0) {
        request->cache_control = value;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    if (request == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    request->response_status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)message;
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    request->response_status = status == 400 ? "400 Bad Request" :
        status == 404 ? "404 Not Found" :
        status == 422 ? "422 Unprocessable Entity" : "500 Internal Server Error";
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    if (request == NULL || buffer == NULL || request->response_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t bytes = length == HTTPD_RESP_USE_STRLEN ? strlen(buffer) : (size_t)length;
    if (bytes >= request->response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(request->response_body, buffer, bytes);
    request->response_body[bytes] = '\0';
    request->response_length = bytes;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    if (request == NULL || request->response_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buffer == NULL && length == 0) {
        if (request->response_length >= request->response_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
        request->response_body[request->response_length] = '\0';
        return ESP_OK;
    }
    if (buffer == NULL || length <= 0 || request->response_length + (size_t)length >= request->response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    return uri == NULL ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)method;
    return uri == NULL ? ESP_ERR_INVALID_ARG : ESP_OK;
}

#ifdef _MSC_VER
#pragma warning(disable: 4005)
#endif
#include "../../web_server/web_json_writer.c"

#define static
#include "../../web_server/web_localization_api.c"

/* The production runs handler now refuses to list while storage is unavailable;
 * this E2E uses a real store in a temp directory, so storage is always ready. */
bool storage_manager_is_ready(void)
{
    return true;
}
#include "../../web_server/web_localization_storage.c"
#undef static

static uint32_t e2e_random_u32(void)
{
    s_e2e_rng_state ^= s_e2e_rng_state << 13U;
    s_e2e_rng_state ^= s_e2e_rng_state >> 17U;
    s_e2e_rng_state ^= s_e2e_rng_state << 5U;
    return s_e2e_rng_state;
}

static int16_t e2e_noise_counts(void)
{
    return (int16_t)((int32_t)(e2e_random_u32() % 21U) - 10);
}

static void e2e_add_wavelet(size_t channel, size_t center, double amplitude_counts)
{
    const size_t half_width = E2E_PULSE_WIDTH_SAMPLES / 2U;
    const size_t first = center > half_width ? center - half_width : 0U;
    const double pi = 3.14159265358979323846;
    for (size_t offset = 0U; offset < E2E_PULSE_WIDTH_SAMPLES && first + offset < s_e2e_frame_count; ++offset) {
        const double relative_s = ((double)offset - (double)half_width) / (double)E2E_SAMPLE_RATE_HZ;
        const double a = pi * E2E_PULSE_FREQUENCY_HZ * relative_s;
        const double wavelet = (1.0 - 2.0 * a * a) * exp(-a * a);
        const int32_t pulse = (int32_t)llround(amplitude_counts * wavelet);
        const int32_t combined = (int32_t)s_e2e_raw_frames[first + offset][channel] + pulse;
        const int32_t bounded = combined > INT16_MAX ? INT16_MAX : combined < INT16_MIN ? INT16_MIN : combined;
        s_e2e_raw_frames[first + offset][channel] = (int16_t)bounded;
    }
}

static void e2e_ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return;
    }
    e2e_require(false, "run-store setup", "could not create a test-owned directory under the host build cwd");
}

static void e2e_prepare_run_store(const char *session_id)
{
    char path[256];
    e2e_ensure_directory(STORAGE_MANAGER_MOUNT_POINT);
    e2e_ensure_directory(STORAGE_MANAGER_ROOT_PATH);
    e2e_ensure_directory(STORAGE_MANAGER_OBJECTS_PATH);
    int written = snprintf(path, sizeof(path), "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, E2E_OBJECT_ID);
    e2e_require(written > 0 && (size_t)written < sizeof(path), "run-store setup", "object path overflow");
    e2e_ensure_directory(path);
    written = snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, E2E_OBJECT_ID);
    e2e_require(written > 0 && (size_t)written < sizeof(path), "run-store setup", "sessions path overflow");
    e2e_ensure_directory(path);
    written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, E2E_OBJECT_ID, session_id
    );
    e2e_require(written > 0 && (size_t)written < sizeof(path), "run-store setup", "session path overflow");
    e2e_ensure_directory(path);

    written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, E2E_OBJECT_ID, session_id
    );
    e2e_require(written > 0 && (size_t)written < sizeof(path), "scan fixture", "occupancy path overflow");
    FILE *file = fopen(path, "wb");
    e2e_require(file != NULL, "scan fixture", "could not create the intentionally invalid synthetic occupancy artifact");
    static const char invalid_scan[] = "synthetic invalid occupancy fixture";
    const bool wrote = fwrite(invalid_scan, 1U, sizeof(invalid_scan), file) == sizeof(invalid_scan);
    const bool closed = fclose(file) == 0;
    e2e_require(wrote && closed, "scan fixture", "could not write the intentionally invalid synthetic occupancy artifact");
}

static void e2e_build_geometry_and_raw_fixture(
    double p_truth_s[AD7606_CHANNEL_COUNT],
    size_t active_sensor_count,
    localization_point3d_t source_position
)
{
    e2e_require(
        active_sensor_count > 0U && active_sensor_count <= AD7606_CHANNEL_COUNT,
        "raw frame decode",
        "synthetic geometry sensor count is outside the production frame layout"
    );
    free(s_e2e_raw_frames);
    free(s_e2e_decoded_frames);
    s_e2e_raw_frames = NULL;
    s_e2e_decoded_frames = NULL;

    memset(&s_e2e_geometry, 0, sizeof(s_e2e_geometry));
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        object_store_pod_geometry_t *pod = &s_e2e_geometry.pods[channel];
        pod->computed = channel < active_sensor_count;
        pod->leveled = channel < active_sensor_count;
        pod->x_mm = (int32_t)llround(s_e2e_layout[channel].x_m * 1000.0);
        pod->y_mm = (int32_t)llround(s_e2e_layout[channel].y_m * 1000.0);
        pod->z_mm = (int32_t)llround(s_e2e_layout[channel].z_m * 1000.0);
        pod->computed_at_ms = UINT64_C(132000) + (uint64_t)channel;

        /* Independent Euclidean geometry and travel-time oracle for this fixture. */
        const double dx = source_position.x_m - s_e2e_layout[channel].x_m;
        const double dy = source_position.y_m - s_e2e_layout[channel].y_m;
        const double dz = source_position.z_m - s_e2e_layout[channel].z_m;
        const double distance_m = sqrt(dx * dx + dy * dy + dz * dz);
        p_truth_s[channel] = E2E_EMISSION_TIME_S + distance_m / E2E_P_SPEED_MPS;
    }

    s_e2e_frame_count = E2E_FRAME_COUNT;
    s_e2e_raw_frames = calloc(s_e2e_frame_count, sizeof(*s_e2e_raw_frames));
    s_e2e_decoded_frames = calloc(s_e2e_frame_count, sizeof(*s_e2e_decoded_frames));
    e2e_require(
        s_e2e_raw_frames != NULL && s_e2e_decoded_frames != NULL,
        "raw frame decode",
        "could not allocate one second of four-channel synthetic AD7606 frames"
    );

    s_e2e_rng_state = UINT32_C(0x13212E2E);
    size_t s_centers[AD7606_CHANNEL_COUNT];
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        const double dx = source_position.x_m - s_e2e_layout[channel].x_m;
        const double dy = source_position.y_m - s_e2e_layout[channel].y_m;
        const double dz = source_position.z_m - s_e2e_layout[channel].z_m;
        const double distance_m = sqrt(dx * dx + dy * dy + dz * dz);
        const double s_truth_s = E2E_EMISSION_TIME_S + distance_m / E2E_S_SPEED_MPS;
        const size_t p_center = (size_t)llround(p_truth_s[channel] * (double)E2E_SAMPLE_RATE_HZ);
        s_centers[channel] = (size_t)llround(s_truth_s * (double)E2E_SAMPLE_RATE_HZ);
        for (size_t sample = 0U; sample < s_e2e_frame_count; ++sample) {
            const int32_t baseline = 100 + (int32_t)(channel * 11U);
            const int32_t with_noise = baseline + (int32_t)e2e_noise_counts();
            s_e2e_raw_frames[sample][channel] = (int16_t)with_noise;
        }
        if (channel < active_sensor_count) {
            e2e_add_wavelet(channel, p_center, 5000.0);
            e2e_add_wavelet(channel, s_centers[channel], 3200.0);
        }
    }

    memset(&s_e2e_session_summary, 0, sizeof(s_e2e_session_summary));
    s_e2e_session_summary.sample_rate_hz = E2E_SAMPLE_RATE_HZ;
    s_e2e_session_summary.channel_count = SEISMIC_CHANNEL_COUNT;
    s_e2e_session_summary.samples_written = s_e2e_frame_count;
    s_e2e_session_summary.complete = true;
    s_e2e_session_summary.readable = true;

    e2e_require(
        E2E_SAMPLE_RATE_HZ == 20000U && s_e2e_session_summary.channel_count == AD7606_CHANNEL_COUNT,
        "raw frame decode",
        "fixture rate or channel layout differs from the production sdkconfig/AD7606 constants"
    );
    e2e_require(
        ad7606_raw_to_uv(8192) == 1250000.0f && ad7606_raw_to_uv(-32768) == -5000000.0f,
        "raw frame decode",
        "production signed-count to microvolt conversion does not match the bipolar 5 V transfer function"
    );

    for (size_t sample = 0U; sample < s_e2e_frame_count; ++sample) {
        s_e2e_decoded_frames[sample].timestamp_us =
            (int64_t)((uint64_t)sample * UINT64_C(1000000) / E2E_SAMPLE_RATE_HZ);
        for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
            s_e2e_decoded_frames[sample].channel_uv[channel] = ad7606_raw_to_uv(
                s_e2e_raw_frames[sample][channel]
            );
        }
    }
    e2e_require(
        s_e2e_decoded_frames[0].channel_uv[0] == ad7606_raw_to_uv(s_e2e_raw_frames[0][0]) &&
            fabs((double)s_e2e_decoded_frames[0].channel_uv[0]) > 10000.0,
        "raw frame decode",
        "raw signed counts were not decoded into the production microvolt engineering-unit frame"
    );
    (void)printf(
        "[MAX-132 E2E] raw: seed=0x13212E2E frames=%zu rate=%u Hz channels=%u scale=152.587890625 uV/count\n",
        s_e2e_frame_count,
        (unsigned)E2E_SAMPLE_RATE_HZ,
        (unsigned)AD7606_CHANNEL_COUNT
    );
}

static void e2e_assert_dc_preprocessing(void)
{
    static float channel_copy[E2E_FRAME_COUNT];
    double input_mean = 0.0;
    for (size_t sample = 0U; sample < s_e2e_frame_count; ++sample) {
        channel_copy[sample] = s_e2e_decoded_frames[sample].channel_uv[0];
        input_mean += (double)channel_copy[sample];
    }
    input_mean /= (double)s_e2e_frame_count;

    const seismic_dsp_config_t dsp_config = {
        .sample_rate_hz = E2E_SAMPLE_RATE_HZ,
        .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL,
    };
    seismic_dsp_result_t dsp_result;
    e2e_require(seismic_dsp_init(&dsp_config) == ESP_OK, "preprocessing", "production DC-only DSP config was rejected");
    e2e_require(
        seismic_dsp_process_block(channel_copy, s_e2e_frame_count, false, &dsp_result) == ESP_OK,
        "preprocessing",
        "production DC-only DSP block failed"
    );
    e2e_require(
        dsp_result.completed_stages == SEISMIC_DSP_STAGE_DC_REMOVAL && dsp_result.placeholder_stages == 0U,
        "preprocessing",
        "fixture did not run exactly the production localization DC-removal stage"
    );
    e2e_require(
        fabs((double)dsp_result.dc_offset_removed - input_mean) < 0.02,
        "preprocessing",
        "production DC-removal offset differs from the decoded frame mean"
    );

    double output_mean = 0.0;
    for (size_t sample = 0U; sample < s_e2e_frame_count; ++sample) {
        output_mean += (double)channel_copy[sample];
    }
    output_mean /= (double)s_e2e_frame_count;
    e2e_require(fabs(output_mean) < 0.02, "preprocessing", "DC-removed synthetic channel retains a nonzero mean");
    (void)printf(
        "[MAX-132 E2E] preprocessing: completed=DC_REMOVAL only, offset=%.2f uV, residual_mean=%.4f uV; no filters configured\n",
        (double)dsp_result.dc_offset_removed,
        output_mean
    );
}

static localization_calculate_config_t e2e_calculation_config(void)
{
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    config.propagation_speed_mps = E2E_P_SPEED_MPS;
    config.propagation_speed_uncertainty_mps = 20.0;
    (void)snprintf(config.propagation_model_name, sizeof(config.propagation_model_name), "MAX-132 synthetic truth");
    config.timing_uncertainty_us = 400.0;
    config.sample_rate_hz = E2E_SAMPLE_RATE_HZ;
    config.pick_config.noise_window_samples = 50U;
    config.pick_config.sta_window_samples = 10U;
    config.pick_config.lta_window_samples = 100U;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    config.pick_config.refine_lookback_samples = 10U;
    config.pick_config.refine_lookahead_samples = 10U;
    config.pick_config.refine_amplitude_fraction = 0.5f;
    config.pick_config.snr_signal_window_samples = E2E_PULSE_WIDTH_SAMPLES;
    config.pick_config.minimum_snr = 3.0f;
    config.semblance_window_half_span_samples = 10U;
    config.max_source_depth_m = 0.0;
    config.s_pick_enabled = true;
    config.s_pick_config.min_separation_samples = 50U;
    config.s_pick_config.max_separation_samples = 1500U;
    config.s_pick_config.sta_window_samples = 10U;
    config.s_pick_config.lta_window_samples = 50U;
    config.s_pick_config.sta_lta_trigger_ratio = 3.0f;
    config.s_pick_config.minimum_snr = 3.0f;
    return config;
}

static const localization_run_record_t *e2e_find_run(
    const localization_run_record_t *records,
    size_t count,
    const char *method_name
)
{
    const localization_run_record_t *found = NULL;
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(records[i].method_name, method_name) == 0) {
            e2e_require(found == NULL, "run-store persistence", "duplicate method record was returned by the real store");
            found = &records[i];
        }
    }
    return found;
}

static void e2e_assert_persisted_picks(const double p_truth_s[AD7606_CHANNEL_COUNT])
{
    localization_pick_record_t pick_records[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t pick_count = 0U;
    e2e_require(
        localization_run_store_list_picks(
            E2E_OBJECT_ID,
            E2E_SESSION_ID,
            pick_records,
            LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION,
            &pick_count
        ) == ESP_OK,
        "event/pick stage",
        "real run store could not reload the production pick record"
    );
    e2e_require(pick_count == 1U, "event/pick stage", "real run store did not return exactly one pick record");
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        const localization_pick_channel_t *entry = &pick_records[0].channels[channel];
        e2e_require(entry->participating, "event/pick stage", "a production geometry channel was not marked participating");
        e2e_require(
            entry->pick.status == GEOPHONE_PICK_ACCEPTED,
            "event/pick stage",
            "production geophone_events picker did not accept a synthetic P arrival"
        );
        const double error_s = fabs((double)entry->pick.arrival_time_s - p_truth_s[channel]);
        if (error_s > E2E_PICK_TOLERANCE_S) {
            (void)fprintf(
                stderr,
                "[MAX-132 E2E FAIL] stage=event/pick stage detail=channel %u error %.6f s exceeds %.6f s\n",
                (unsigned)(channel + 1U), error_s, E2E_PICK_TOLERANCE_S
            );
            exit(EXIT_FAILURE);
        }
        (void)printf(
            "[MAX-132 E2E] pick: ch=%u truth=%.6f s picked=%.6f s error=%.6f s tolerance=%.6f s\n",
            (unsigned)(channel + 1U),
            p_truth_s[channel],
            (double)entry->pick.arrival_time_s,
            error_s,
            E2E_PICK_TOLERANCE_S
        );
    }
}

static size_t e2e_assert_run_store(
    localization_calculate_summary_t *summary_out,
    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION]
)
{
    e2e_require(summary_out->methods_total == 21U, "localization run", "eligible method inventory is not 21 records");
    e2e_require(summary_out->methods_completed == 21U, "localization run", "the production run did not complete every method boundary");
    e2e_require(summary_out->methods_persisted == 21U, "localization run", "the production run did not persist every result");

    size_t count = 0U;
    e2e_require(
        localization_run_store_list(
            E2E_OBJECT_ID,
            E2E_SESSION_ID,
            records,
            LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION,
            &count
        ) == ESP_OK,
        "run-store persistence",
        "real run store could not reload the production method records"
    );
    e2e_require(count == 21U, "run-store persistence", "expected 20 individual records plus one separate joint record");

    size_t individual_count = 0U;
    size_t joint_count = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (records[i].kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD) {
            ++individual_count;
        } else if (records[i].kind == LOCALIZATION_RUN_KIND_JOINT_FUSION) {
            ++joint_count;
        }
        e2e_require(
            records[i].object_id[0] != '\0' && records[i].session_id[0] != '\0' && records[i].method_name[0] != '\0',
            "run-store persistence",
            "a persisted record is missing its identity fields"
        );
        for (size_t j = i + 1U; j < count; ++j) {
            e2e_require(
                strcmp(records[i].method_name, records[j].method_name) != 0,
                "run-store persistence",
                "individual method outputs were overwritten or duplicated"
            );
        }
    }
    e2e_require(individual_count == 20U && joint_count == 1U, "run-store persistence", "individual/joint record kinds were conflated");

    for (size_t i = 0U; i < sizeof(s_e2e_method_expectations) / sizeof(s_e2e_method_expectations[0]); ++i) {
        const e2e_method_expectation_t *expected = &s_e2e_method_expectations[i];
        const localization_run_record_t *record = e2e_find_run(records, count, expected->name);
        e2e_require(record != NULL, "run-store persistence", "a D3 matrix individual method record is absent");
        e2e_require(
            record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD,
            "run-store persistence",
            "an individual method was serialized as a joint result"
        );
        e2e_require(
            record->solved == expected->solved_4_sensor_3d,
            "run-store persistence",
            "method solved/skipped status differs from the D3 four-sensor matrix"
        );
        if (!record->solved) {
            e2e_require(record->rejection_reason[0] != '\0', "run-store persistence", "a skipped individual method has no reason");
        }
        (void)printf("[MAX-132 E2E] method: %-28s %s%s%s\n", record->method_name,
            record->solved ? "SOLVED" : "SKIPPED", record->solved ? "" : " reason=", record->solved ? "" : record->rejection_reason);
    }

    for (size_t i = 0U; i < sizeof(s_e2e_scan_methods) / sizeof(s_e2e_scan_methods[0]); ++i) {
        const localization_run_record_t *record = e2e_find_run(records, count, s_e2e_scan_methods[i]);
        e2e_require(record != NULL, "run-store persistence", "an eligible scan method record is absent");
        e2e_require(
            record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD && !record->solved && record->rejection_reason[0] != '\0',
            "run-store persistence",
            "scan method did not retain its independent skipped record and reason"
        );
        (void)printf("[MAX-132 E2E] method: %-28s SKIPPED reason=%s\n", record->method_name, record->rejection_reason);
    }

    const localization_run_record_t *joint = e2e_find_run(records, count, "joint_fusion");
    e2e_require(
        joint != NULL && joint->kind == LOCALIZATION_RUN_KIND_JOINT_FUSION,
        "run-store persistence",
        "joint fusion was not persisted as its separate joint record"
    );
    char individual_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    char joint_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const localization_run_record_t *tdoa = e2e_find_run(records, count, "tdoa_gauss_newton");
    e2e_require(tdoa != NULL, "run-store persistence", "TDOA record is required for joint path separation check");
    e2e_require(
        localization_run_store_build_path(E2E_OBJECT_ID, E2E_SESSION_ID, tdoa, individual_path, sizeof(individual_path)) == ESP_OK &&
            localization_run_store_build_path(E2E_OBJECT_ID, E2E_SESSION_ID, joint, joint_path, sizeof(joint_path)) == ESP_OK &&
            strcmp(individual_path, joint_path) != 0,
        "run-store persistence",
        "individual and joint records resolve to the same persisted path"
    );
    (void)printf("[MAX-132 E2E] run-store: %zu records (%zu individual, %zu joint); each method has an independent path\n", count, individual_count, joint_count);
    return count;
}

static void e2e_assert_three_sensor_method_matrix(void)
{
    const localization_point3d_t planar_source = { 0.70, 0.80, 0.0 };
    double p_truth_s[AD7606_CHANNEL_COUNT] = { 0.0 };
    e2e_prepare_run_store(E2E_PLANAR_MATRIX_SESSION_ID);
    e2e_build_geometry_and_raw_fixture(p_truth_s, 3U, planar_source);
    e2e_assert_dc_preprocessing();

    localization_calculate_config_t config = e2e_calculation_config();
    localization_calculate_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    e2e_require(
        localization_calculate_run(
            E2E_OBJECT_ID, E2E_PLANAR_MATRIX_SESSION_ID, &config, E2E_COMPUTED_AT_MS + 1U, &summary
        ) == ESP_OK,
        "3-sensor D3 matrix",
        "production localization_calculate_run did not complete for the three-sensor planar fixture"
    );
    e2e_require(
        summary.participating_channel_count == 3U && summary.methods_total == 21U &&
            summary.methods_completed == 21U && summary.methods_persisted == 21U,
        "3-sensor D3 matrix",
        "three-sensor run did not use three participants and persist all 21 records"
    );

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    e2e_require(
        localization_run_store_list(
            E2E_OBJECT_ID, E2E_PLANAR_MATRIX_SESSION_ID, records,
            LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count
        ) == ESP_OK && count == 21U,
        "3-sensor D3 matrix",
        "real run store did not reload the three-sensor matrix's 21 records"
    );
    for (size_t i = 0U; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            e2e_require(
                strcmp(records[i].method_name, records[j].method_name) != 0,
                "3-sensor D3 matrix",
                "three-sensor individual or joint records were overwritten or duplicated"
            );
        }
    }

    for (size_t i = 0U; i < sizeof(s_e2e_method_expectations) / sizeof(s_e2e_method_expectations[0]); ++i) {
        const e2e_method_expectation_t *expected = &s_e2e_method_expectations[i];
        const localization_run_record_t *record = e2e_find_run(records, count, expected->name);
        e2e_require(
            record != NULL && record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD &&
                record->solved == expected->solved_3_sensor_planar,
            "3-sensor D3 matrix",
            "method solved/skipped status differs from the D3 three-sensor planar matrix"
        );
        if (!record->solved) {
            e2e_require(
                record->rejection_reason[0] != '\0',
                "3-sensor D3 matrix",
                "skipped three-sensor method has no persisted reason"
            );
        }
        (void)printf(
            "[MAX-132 E2E] 3-sensor method: %-28s %s%s%s\n", record->method_name,
            record->solved ? "SOLVED" : "SKIPPED", record->solved ? "" : " reason=",
            record->solved ? "" : record->rejection_reason
        );
    }
    for (size_t i = 0U; i < sizeof(s_e2e_scan_methods) / sizeof(s_e2e_scan_methods[0]); ++i) {
        const localization_run_record_t *record = e2e_find_run(records, count, s_e2e_scan_methods[i]);
        e2e_require(
            record != NULL && record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD &&
                !record->solved && record->rejection_reason[0] != '\0',
            "3-sensor D3 matrix",
            "three-sensor scan method lacks its independently persisted skipped record and reason"
        );
    }
    const localization_run_record_t *joint = e2e_find_run(records, count, "joint_fusion");
    e2e_require(
        joint != NULL && joint->kind == LOCALIZATION_RUN_KIND_JOINT_FUSION && joint->solved,
        "3-sensor D3 matrix",
        "three-sensor joint result is not a separate solved record"
    );
    (void)printf("[MAX-132 E2E] 3-sensor D3 matrix: 20 individual records plus separate joint record match\n");
}

static cJSON *e2e_json_item(cJSON *root, const char *key, const char *stage)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    e2e_require(item != NULL, stage, "required JSON member is missing");
    return item;
}

static cJSON *e2e_json_find_run(cJSON *runs, const char *method_name, const char *stage)
{
    const int run_count = cJSON_GetArraySize(runs);
    for (int i = 0; i < run_count; ++i) {
        cJSON *run = cJSON_GetArrayItem(runs, i);
        cJSON *method = cJSON_GetObjectItemCaseSensitive(run, "method_name");
        if (cJSON_IsString(method) && strcmp(method->valuestring, method_name) == 0) {
            return run;
        }
    }
    e2e_require(false, stage, "required method record is missing from JSON");
    return NULL;
}

static void e2e_assert_http_json(
    size_t expected_run_count,
    const localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION]
)
{
    memset(s_e2e_http_response, 0, sizeof(s_e2e_http_response));
    httpd_req_t request = {
        .uri = "/api/localization/runs",
        .query = "object_id=" E2E_OBJECT_ID "&session_id=" E2E_SESSION_ID,
        .response_body = s_e2e_http_response,
        .response_capacity = sizeof(s_e2e_http_response),
    };
    e2e_require(web_localization_runs_handler(&request) == ESP_OK, "Web localization API", "production listing handler did not return success");
    e2e_require(
        request.content_type != NULL && strstr(request.content_type, "application/json") != NULL &&
            request.response_length > 0U && s_e2e_http_response[request.response_length] == '\0',
        "Web localization API",
        "browser response is missing its JSON content type or body"
    );

    cJSON *root = cJSON_Parse(s_e2e_http_response);
    e2e_require(cJSON_IsObject(root), "Web localization API", "production handler returned malformed listing JSON");
    cJSON *object_id = e2e_json_item(root, "object_id", "Web localization API");
    cJSON *session_id = e2e_json_item(root, "session_id", "Web localization API");
    cJSON *runs = e2e_json_item(root, "runs", "Web localization API");
    cJSON *picks = e2e_json_item(root, "picks", "Web localization API");
    e2e_require(cJSON_IsString(object_id) && strcmp(object_id->valuestring, E2E_OBJECT_ID) == 0,
        "Web localization API", "browser JSON has the wrong object identity");
    e2e_require(cJSON_IsString(session_id) && strcmp(session_id->valuestring, E2E_SESSION_ID) == 0,
        "Web localization API", "browser JSON has the wrong session identity");
    e2e_require(cJSON_IsArray(runs) && (size_t)cJSON_GetArraySize(runs) == expected_run_count,
        "Web localization API", "browser JSON did not list every real persisted method record");
    e2e_require(cJSON_IsArray(picks) && cJSON_GetArraySize(picks) == 1,
        "Web localization API", "browser JSON did not list the real persisted pick record");

    for (size_t i = 0U; i < expected_run_count; ++i) {
        cJSON *run = e2e_json_find_run(runs, records[i].method_name, "Web localization API");
        cJSON *kind = e2e_json_item(run, "kind", "Web localization API");
        cJSON *solved = e2e_json_item(run, "solved", "Web localization API");
        cJSON *reason = cJSON_GetObjectItemCaseSensitive(run, "rejection_reason");
        e2e_require(cJSON_IsString(kind), "Web localization API", "run kind is malformed in the browser JSON");
        e2e_require(cJSON_IsBool(solved), "Web localization API", "solved status is malformed in the browser JSON");
        if (!records[i].solved) {
            e2e_require(
                cJSON_IsString(reason) && reason->valuestring != NULL && reason->valuestring[0] != '\0',
                "Web localization API",
                "a skipped method's reason was lost in the browser JSON"
            );
        }
    }
    cJSON *joint = e2e_json_find_run(runs, "joint_fusion", "Web localization API");
    cJSON *joint_kind = e2e_json_item(joint, "kind", "Web localization API");
    e2e_require(
        cJSON_IsString(joint_kind) && strcmp(joint_kind->valuestring, "joint_fusion") == 0,
        "Web localization API",
        "joint record is not distinct in the browser JSON"
    );
    cJSON *pick_record = cJSON_GetArrayItem(picks, 0);
    cJSON *channels = e2e_json_item(pick_record, "channels", "Web localization API");
    e2e_require(
        cJSON_IsArray(channels) && cJSON_GetArraySize(channels) == AD7606_CHANNEL_COUNT,
        "Web localization API",
        "browser JSON did not retain all four channel picks"
    );
    for (int channel = 0; channel < cJSON_GetArraySize(channels); ++channel) {
        cJSON *entry = cJSON_GetArrayItem(channels, channel);
        cJSON *status = e2e_json_item(entry, "status", "Web localization API");
        e2e_require(
            cJSON_IsString(status) && strcmp(status->valuestring, "accepted") == 0,
            "Web localization API",
            "browser JSON changed or dropped an accepted production pick"
        );
    }

    e2e_require(
        cJSON_GetArraySize(runs) == 21 && cJSON_IsArray(e2e_json_item(root, "picks", "Web localization API")),
        "Web localization API",
        "success response shape omitted records or picks"
    );
    (void)printf("[MAX-132 E2E] Web API: real handler emitted %d runs and %d pick record(s) to the browser\n",
        cJSON_GetArraySize(runs), cJSON_GetArraySize(picks));
    cJSON_Delete(root);
}

static bool e2e_is_volatile_number_key(const char *key)
{
    static const char *const keys[] = {
        "computed_at_ms", "geometry_computed_at_ms", "start_uptime_ms",
        "source_sample_uptime_ms", "esp_receipt_uptime_ms", "captured_at_ms",
        "computation_time_ms", "computation_time_us", "elapsed_ms",
    };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strcmp(key, keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void e2e_normalize_volatile_fields(cJSON *node)
{
    if (node == NULL || (!cJSON_IsObject(node) && !cJSON_IsArray(node))) {
        return;
    }
    for (cJSON *child = node->child; child != NULL; child = child->next) {
        if (child->string != NULL && e2e_is_volatile_number_key(child->string) && cJSON_IsNumber(child)) {
            child->valuedouble = 1.0;
            child->valueint = 1;
        } else {
            e2e_normalize_volatile_fields(child);
        }
    }
}

static char *e2e_read_file(const char *path, const char *stage)
{
    FILE *file = fopen(path, "rb");
    e2e_require(file != NULL, stage, "could not open the frozen JSON fixture");
    e2e_require(fseek(file, 0L, SEEK_END) == 0, stage, "could not seek the frozen JSON fixture");
    const long file_length = ftell(file);
    e2e_require(file_length >= 0L && fseek(file, 0L, SEEK_SET) == 0, stage, "could not size/read the frozen JSON fixture");
    char *text = malloc((size_t)file_length + 1U);
    e2e_require(text != NULL, stage, "could not allocate the frozen JSON fixture buffer");
    const size_t read_count = fread(text, 1U, (size_t)file_length, file);
    const bool closed = fclose(file) == 0;
    e2e_require(read_count == (size_t)file_length && closed, stage, "frozen JSON fixture read was incomplete");
    text[read_count] = '\0';
    return text;
}

static void e2e_write_build_observation(const char *normalized_json)
{
    static const char output_directory[] = "../../../.test-build";
    static const char output_path[] = "../../../.test-build/max132_e2e_actual.json";
    if (mkdir(output_directory, 0775) != 0 && errno != EEXIST) {
        e2e_require(false, "golden JSON", "could not create .test-build for the actual observation");
    }
    FILE *file = fopen(output_path, "wb");
    e2e_require(file != NULL, "golden JSON", "could not write normalized API JSON under .test-build/");
    const size_t length = strlen(normalized_json);
    const bool wrote = fwrite(normalized_json, 1U, length, file) == length;
    const bool closed = fclose(file) == 0;
    e2e_require(wrote && closed, "golden JSON", "could not persist normalized API JSON under .test-build/");
}

static void e2e_assert_golden_json(void)
{
    cJSON *actual_tree = cJSON_Parse(s_e2e_http_response);
    e2e_require(cJSON_IsObject(actual_tree), "golden JSON", "browser response could not be parsed for golden comparison");
    e2e_normalize_volatile_fields(actual_tree);
    char *actual_normalized = cJSON_PrintUnformatted(actual_tree);
    e2e_require(actual_normalized != NULL, "golden JSON", "normalized browser response could not be serialized");
    e2e_write_build_observation(actual_normalized);

    static const char golden_path[] = "../../../tests/host/data/max132_e2e_runs.golden.json";
    char *golden_text = e2e_read_file(golden_path, "golden JSON");
    cJSON *golden_tree = cJSON_Parse(golden_text);
    e2e_require(cJSON_IsObject(golden_tree), "golden JSON", "frozen fixture is malformed JSON");
    e2e_normalize_volatile_fields(golden_tree);
    char *golden_normalized = cJSON_PrintUnformatted(golden_tree);
    e2e_require(golden_normalized != NULL, "golden JSON", "frozen fixture could not be normalized");
    if (strcmp(actual_normalized, golden_normalized) != 0) {
        e2e_require(false, "golden JSON", "normalized production API response differs from the frozen JSON fixture; see .test-build/max132_e2e_actual.json");
    }
    (void)printf("[MAX-132 E2E] golden JSON: normalized production browser response matches the frozen fixture\n");

    cJSON_free(golden_normalized);
    cJSON_Delete(golden_tree);
    free(golden_text);
    cJSON_free(actual_normalized);
    cJSON_Delete(actual_tree);
}

/* One full run of the production localization; the source the plain TDOA method placed (or "not solved"). */
static bool e2e_run_and_read_tdoa_source(const char *session_id, uint64_t computed_at_ms, localization_point3d_t *source)
{
    localization_calculate_config_t config = e2e_calculation_config();
    localization_calculate_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    e2e_require(
        localization_calculate_run(E2E_OBJECT_ID, session_id, &config, computed_at_ms, &summary) == ESP_OK,
        "REMOTE_ONLY registered positions", "production localization_calculate_run did not complete");
    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    e2e_require(
        localization_run_store_list(E2E_OBJECT_ID, session_id, records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count) == ESP_OK &&
            count > 0U,
        "REMOTE_ONLY registered positions", "run store did not reload the records");
    const localization_run_record_t *tdoa = e2e_find_run(records, count, "tdoa_gauss_newton");
    e2e_require(tdoa != NULL, "REMOTE_ONLY registered positions", "TDOA record is absent");
    if (tdoa->solved) *source = tdoa->source_position;
    return tdoa->solved;
}

static double e2e_distance_m(const localization_point3d_t *a, const localization_point3d_t *b)
{
    return sqrt((a->x_m - b->x_m) * (a->x_m - b->x_m) + (a->y_m - b->y_m) * (a->y_m - b->y_m) + (a->z_m - b->z_m) * (a->z_m - b->z_m));
}

/*
 * REMOTE_ONLY end to end: the sensors and the source are rotated and shifted into a "scan frame" that Channel Sounding does not
 * know. The signals come from that truth. With the registered tag-centre positions the production pipeline (no MAIN, no tag 0
 * anywhere) places the source there; with only the Channel Sounding layout (the old frame) it cannot.
 */
static void e2e_assert_remote_only_registered_positions(void)
{
    static localization_point3d_t scan_layout[AD7606_CHANNEL_COUNT];
    const double yaw_rad = 37.0 * 3.14159265358979323846 / 180.0;
    const double c = cos(yaw_rad), sn = sin(yaw_rad);
    const double tx = 3.2, ty = -1.1, tz = 0.4;
    for (size_t i = 0U; i < AD7606_CHANNEL_COUNT; ++i) {
        scan_layout[i] = (localization_point3d_t){
            c * s_e2e_sensor_positions[i].x_m - sn * s_e2e_sensor_positions[i].y_m + tx,
            sn * s_e2e_sensor_positions[i].x_m + c * s_e2e_sensor_positions[i].y_m + ty,
            s_e2e_sensor_positions[i].z_m + tz };
    }
    const localization_point3d_t scan_source = {
        c * s_e2e_source_position.x_m - sn * s_e2e_source_position.y_m + tx,
        sn * s_e2e_source_position.x_m + c * s_e2e_source_position.y_m + ty,
        s_e2e_source_position.z_m + tz };

    /* The signals of the scan-frame truth; s_e2e_geometry is then that same scan-frame layout. */
    double p_truth_s[AD7606_CHANNEL_COUNT] = { 0.0 };
    s_e2e_layout = scan_layout;
    e2e_build_geometry_and_raw_fixture(p_truth_s, AD7606_CHANNEL_COUNT, scan_source);
    s_e2e_layout = s_e2e_sensor_positions;

    memset(&s_e2e_registered_positions, 0, sizeof(s_e2e_registered_positions));
    s_e2e_registered_positions.available = true;
    uint64_t geometry_computed_at_ms = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        s_e2e_registered_positions.nodes[i] = (seismic_recorder_registered_position_t){
            .valid = true, .node_id = (uint8_t)(i + 1U), .tag_id = (uint8_t)(i + 1U),
            .x_mm = s_e2e_geometry.pods[i].x_mm, .y_mm = s_e2e_geometry.pods[i].y_mm, .z_mm = s_e2e_geometry.pods[i].z_mm };
        if (s_e2e_geometry.pods[i].computed_at_ms > geometry_computed_at_ms) geometry_computed_at_ms = s_e2e_geometry.pods[i].computed_at_ms;
    }
    s_e2e_registered_positions.geometry_computed_at_ms = geometry_computed_at_ms;

    /* What Channel Sounding froze: the same four sensors in the unrotated frame of the fixed layout. */
    s_e2e_channel_sounding_layout = s_e2e_geometry;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        s_e2e_channel_sounding_layout.pods[i].x_mm = (int32_t)llround(s_e2e_sensor_positions[i].x_m * 1000.0);
        s_e2e_channel_sounding_layout.pods[i].y_mm = (int32_t)llround(s_e2e_sensor_positions[i].y_m * 1000.0);
        s_e2e_channel_sounding_layout.pods[i].z_mm = (int32_t)llround(s_e2e_sensor_positions[i].z_m * 1000.0);
    }

    localization_point3d_t placed = { 0.0, 0.0, 0.0 };
    s_e2e_geometry_mode = E2E_GEOMETRY_REGISTERED_OVER_CHANNEL_SOUNDING;
    e2e_prepare_run_store(E2E_REMOTE_ONLY_SESSION_ID);
    e2e_require(e2e_run_and_read_tdoa_source(E2E_REMOTE_ONLY_SESSION_ID, E2E_COMPUTED_AT_MS + 2U, &placed),
        "REMOTE_ONLY registered positions", "the registered positions did not give a TDOA solution");
    const double registered_error_m = e2e_distance_m(&placed, &scan_source);
    (void)printf("[MAX-132 E2E] REMOTE_ONLY registered positions: source error %.4f m in the scan frame\n", registered_error_m);

    localization_point3d_t unplaced = { 0.0, 0.0, 0.0 };
    s_e2e_geometry_mode = E2E_GEOMETRY_CHANNEL_SOUNDING_ONLY;
    e2e_prepare_run_store(E2E_REMOTE_ONLY_CONTROL_SESSION_ID);
    const bool control_solved = e2e_run_and_read_tdoa_source(E2E_REMOTE_ONLY_CONTROL_SESSION_ID, E2E_COMPUTED_AT_MS + 3U, &unplaced);
    const double control_error_m = control_solved ? e2e_distance_m(&unplaced, &scan_source) : INFINITY;
    (void)printf("[MAX-132 E2E] REMOTE_ONLY control (Channel Sounding frame only): %s, error %.4f m\n",
        control_solved ? "solved" : "not solved", control_error_m);
    s_e2e_geometry_mode = E2E_GEOMETRY_AS_FIXTURE;

    e2e_require(registered_error_m < E2E_REMOTE_ONLY_TOLERANCE_M, "REMOTE_ONLY registered positions",
        "the source is not where the scan-frame truth puts it");
    e2e_require(control_error_m > 10.0 * E2E_REMOTE_ONLY_TOLERANCE_M, "REMOTE_ONLY registered positions",
        "the control run (wrong frame) is not clearly worse, so the registered positions prove nothing");
}

int main(void)
{
    double p_truth_s[AD7606_CHANNEL_COUNT] = { 0.0 };
    e2e_prepare_run_store(E2E_SESSION_ID);
    e2e_build_geometry_and_raw_fixture(p_truth_s, AD7606_CHANNEL_COUNT, s_e2e_source_position);
    e2e_assert_dc_preprocessing();

    localization_calculate_config_t config = e2e_calculation_config();
    localization_calculate_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    const esp_err_t run_result = localization_calculate_run(
        E2E_OBJECT_ID,
        E2E_SESSION_ID,
        &config,
        E2E_COMPUTED_AT_MS,
        &summary
    );
    e2e_require(run_result == ESP_OK, "localization run", "production localization_calculate_run did not complete");
    e2e_require(summary.participating_channel_count == AD7606_CHANNEL_COUNT,
        "localization run", "production run did not use all four synthetic sensor channels");
    e2e_assert_persisted_picks(p_truth_s);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    const size_t record_count = e2e_assert_run_store(&summary, records);
    e2e_assert_http_json(record_count, records);
    e2e_assert_golden_json();
    e2e_assert_three_sensor_method_matrix();
    e2e_assert_remote_only_registered_positions();

    free(s_e2e_raw_frames);
    free(s_e2e_decoded_frames);
    (void)printf("[MAX-132 E2E] PASS: synthetic software chain complete; this is not physical proof\n");
    return EXIT_SUCCESS;
}
