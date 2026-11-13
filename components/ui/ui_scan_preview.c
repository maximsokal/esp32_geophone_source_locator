#include "ui_internal.h"
#include "ui_utf8_format.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_task_config.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(ESP_PLATFORM)
#include "freertos/idf_additions.h"
#endif

/*
 * The stack of this task lives in PSRAM: internal RAM is nearly used up once Wi-Fi, the web server and the UI are up
 * (board log 2026-11-13: 5 KB free at boot), and the task touches neither flash nor DMA buffers on its stack.
 */
static BaseType_t ui_scan_preview_task_create(
    void (*function)(void *), const char *name, unsigned stack_bytes, void *argument,
    unsigned priority, TaskHandle_t *handle, int core)
{
#if defined(ESP_PLATFORM)
    return xTaskCreatePinnedToCoreWithCaps(
        function, name, stack_bytes, argument, priority, handle, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return xTaskCreatePinnedToCore(function, name, stack_bytes, argument, priority, handle, core);
#endif
}
#include "localization_calculate_policy.h"
#include "localization_run_store.h"
#include "scan_preview_state.h"
#include "seismic_recorder.h"
#include "storage_bulk_admission.h"
#include "storage_manager.h"
#include "ui_preview3d.h"
#include "ui_scan_preview_policy.h"
#include "ui_workflow_model.h"
#include "workflow_service.h"

#define PREVIEW_WIDTH 320U
#define PREVIEW_HEIGHT 182U
#define PREVIEW_IO_CHUNK 4096U
#define PREVIEW_PATH_SIZE 224U
#define SCAN_FILE_MAX_BYTES (8U * 1024U * 1024U)

static const char *TAG = "UI_SCAN_PREVIEW";

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0U) {
        return;
    }

    const size_t copy_length = strnlen(source, destination_size - 1U);
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

typedef struct
{
    uint32_t generation;
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} preview_request_t;

typedef struct
{
    scan_preview_screen_status_t status;
    scan_preview_t preview;
    scan_occupancy_t occupancy;
    object_store_geometry_t geometry;
    uint64_t geometry_computed_at_ms;
    uint32_t *draw_order;
    ui_preview3d_source_t sources[UI_PREVIEW3D_MAX_SOURCES];
    size_t source_count;
    bool sensor_estimates_fallback;
    char fallback_reason[40];
} preview_result_t;

typedef struct
{
    const char *path;
    long size;
} file_size_read_t;

typedef struct
{
    const char *path;
    size_t offset;
    size_t requested;
    uint8_t *buffer;
} file_chunk_read_t;

typedef struct
{
    const preview_request_t *request;
    object_store_geometry_t *geometry;
} geometry_read_t;

typedef struct
{
    const preview_request_t *request;
    localization_run_record_t *records;
    size_t record_count;
} source_runs_read_t;

static scan_preview_state_t s_state;
static bool s_state_initialized;
static atomic_uint s_active_generation;
static portMUX_TYPE s_request_mux = portMUX_INITIALIZER_UNLOCKED;
static preview_request_t s_pending_request;
static bool s_request_pending;
static TaskHandle_t s_worker_task;
static lv_obj_t *s_canvas;
static lv_obj_t *s_status_label;
static lv_obj_t *s_info_label;
static lv_obj_t *s_legend_label;
static uint8_t *s_canvas_storage;
static uint8_t *s_canvas_pixels;
static uint32_t *s_draw_order;
static ui_preview3d_view_t s_view;
static char s_target_object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
static char s_target_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
static char s_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
static object_store_geometry_t s_geometry;
static uint64_t s_geometry_computed_at_ms;
static ui_preview3d_source_t s_sources[UI_PREVIEW3D_MAX_SOURCES];
static size_t s_source_count;
static bool s_sensor_estimates_fallback;
static bool s_from_workflow;
static bool s_workflow_recording;
static bool s_stop_armed;
static uint64_t s_stop_armed_until_ms;
static bool s_stop_pending;
static uint32_t s_stop_seq;
static ui_i18n_key_t s_stop_notice_key;
static uint64_t s_stop_notice_until_ms;
static uint64_t s_workflow_status_ms;

static void preview_set_footer(ui_i18n_key_t key)
{
    ui_context_t *context = ui_context();
    if (context->footer_label != NULL && key < UI_I18N_KEY_COUNT) {
        lv_label_set_text(context->footer_label, ui_tr(key));
    }
}

static bool request_is_current(const preview_request_t *request)
{
    return request != NULL && atomic_load_explicit(&s_active_generation, memory_order_acquire) == request->generation;
}

static bool safe_id(const char *value, size_t capacity)
{
    if (value == NULL || value[0] == '\0') return false;
    for (size_t i = 0U; i < capacity && value[i] != '\0'; ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return strnlen(value, capacity) < capacity;
}

static bool make_path(char path[PREVIEW_PATH_SIZE], const preview_request_t *request, const char *file_name)
{
    const int length = snprintf(path, PREVIEW_PATH_SIZE, "%s/%s/sessions/%s/%s",
                                STORAGE_MANAGER_OBJECTS_PATH, request->object_id, request->session_id, file_name);
    return length > 0 && (size_t)length < PREVIEW_PATH_SIZE;
}

static esp_err_t file_size_callback(void *context)
{
    file_size_read_t *read = context;
    struct stat info;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    errno = 0;
    if (stat(read->path, &info) != 0) err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    else if (info.st_size <= 0 || info.st_size > LONG_MAX || !S_ISREG(info.st_mode)) err = ESP_ERR_INVALID_SIZE;
    else read->size = (long)info.st_size;
    storage_manager_operation_end();
    return err;
}

static esp_err_t file_chunk_callback(void *context)
{
    file_chunk_read_t *read = context;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    FILE *file = fopen(read->path, "rb");
    if (file == NULL) {
        err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else {
        const bool seek_ok = read->offset <= (size_t)LONG_MAX && fseek(file, (long)read->offset, SEEK_SET) == 0;
        const size_t count = seek_ok ? fread(read->buffer, 1U, read->requested, file) : 0U;
        const bool read_ok = seek_ok && count == read->requested && !ferror(file);
        (void)fclose(file);
        err = read_ok ? ESP_OK : ESP_FAIL;
    }
    storage_manager_operation_end();
    return err;
}

static esp_err_t storage_chunk(const preview_request_t *request, esp_err_t (*callback)(void *), void *context)
{
    if (!request_is_current(request) || storage_bulk_admission_is_protected()) return ESP_ERR_INVALID_STATE;
    return storage_bulk_admission_run_chunk(callback, context);
}

static esp_err_t session_geometry_callback(void *context)
{
    geometry_read_t *read = context;
    return seismic_recorder_read_session_geometry(
        read->request->object_id,
        read->request->session_id,
        read->geometry
    );
}

static esp_err_t source_runs_callback(void *context)
{
    source_runs_read_t *read = context;
    if (!request_is_current(read->request)) return ESP_ERR_INVALID_STATE;
    return localization_run_store_list(
        read->request->object_id,
        read->request->session_id,
        read->records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION,
        &read->record_count
    );
}

/* `scan_identity` is "scan-crc32:%08x" of the validated current scan, or NULL
 * when it could not be established (scan-constrained runs are then excluded). */
static void load_sources(const preview_request_t *request, preview_result_t *result, const char *scan_identity)
{
    const size_t capacity = LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION;
    localization_run_record_t *records = heap_caps_malloc(capacity * sizeof(*records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (records == NULL) return;
    source_runs_read_t read = {.request = request, .records = records, .record_count = 0U};
    const esp_err_t err = storage_chunk(request, source_runs_callback, &read);
    if (err == ESP_OK && read.record_count > 0U) {
        ui_preview3d_source_candidate_t *candidates = heap_caps_calloc(
            read.record_count, sizeof(*candidates), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (candidates != NULL) {
            size_t candidate_count = 0U;
            size_t history_only = 0U;
            for (size_t i = 0U; i < read.record_count; ++i) {
                const localization_run_record_t *run = &records[i];
                /* MAX-414: constrained runs are current only for the scan they were solved against.
                 * Others stay in storage as history and never become the overlaid source. */
                const ui_scan_preview_run_binding_t binding = {
                    .method_name = run->method_name,
                    .scan_provenance_present = run->scan_provenance_present,
                    .scan_constraint_applied = run->scan_constraint_applied,
                    .scan_frame_id = memchr(run->scan_frame_id, '\0', sizeof(run->scan_frame_id)) != NULL ?
                        run->scan_frame_id : NULL,
                };
                if (ui_scan_preview_run_eligibility(&binding, scan_identity) != UI_SCAN_PREVIEW_RUN_ELIGIBLE) {
                    ++history_only;
                    continue;
                }
                candidates[candidate_count++] = (ui_preview3d_source_candidate_t){
                    .method_name = run->method_name,
                    .computed_at_ms = run->computed_at_ms,
                    .confidence_score = run->confidence_score,
                    .position = { (float)run->source_position.x_m, (float)run->source_position.y_m, (float)run->source_position.z_m },
                    .horizontal_std_x_m = run->uncertainty.std_m.x_m,
                    .horizontal_std_y_m = run->uncertainty.std_m.y_m,
                    .vertical_std_m = run->uncertainty.std_m.z_m,
                    .geometry_computed_at_ms = run->geometry_computed_at_ms,
                    .joint_fusion = run->kind == LOCALIZATION_RUN_KIND_JOINT_FUSION,
                    /* The same eligibility as the TFT: a run whose arrivals contradict the velocity range is never shown as a source. */
                    .solved = localization_run_record_is_primary_candidate(run),
                    .uncertainty_too_high = run->uncertainty_too_high,
                    .uncertainty_valid = run->uncertainty.credible_level > 0.0,
                };
            }
            if (history_only > 0U) {
                ESP_LOGI(TAG, "PREVIEW_SOURCES history_only=%u of %u (scan-constrained, not bound to the current scan)",
                         (unsigned)history_only, (unsigned)read.record_count);
            }
            result->source_count = ui_preview3d_select_sources(
                candidates, candidate_count, result->sources, UI_PREVIEW3D_MAX_SOURCES);
            heap_caps_free(candidates);
        }
    }
    heap_caps_free(records);
}

static esp_err_t read_file_size(const preview_request_t *request, const char *name, size_t maximum, size_t *size_out)
{
    char path[PREVIEW_PATH_SIZE];
    if (!make_path(path, request, name)) return ESP_ERR_INVALID_SIZE;
    file_size_read_t read = {.path = path, .size = 0L};
    esp_err_t err = storage_chunk(request, file_size_callback, &read);
    if (err != ESP_OK) return err;
    if ((unsigned long)read.size > maximum) return ESP_ERR_INVALID_SIZE;
    *size_out = (size_t)read.size;
    return ESP_OK;
}

static esp_err_t read_file_chunks(const preview_request_t *request, const char *name, size_t size, uint8_t *buffer)
{
    char path[PREVIEW_PATH_SIZE];
    if (!make_path(path, request, name)) return ESP_ERR_INVALID_SIZE;
    for (size_t offset = 0U; offset < size; offset += PREVIEW_IO_CHUNK) {
        if (!request_is_current(request)) return ESP_ERR_INVALID_STATE;
        const size_t remaining = size - offset;
        file_chunk_read_t read = {
            .path = path,
            .offset = offset,
            .requested = remaining < PREVIEW_IO_CHUNK ? remaining : PREVIEW_IO_CHUNK,
            .buffer = buffer + offset,
        };
        const esp_err_t err = storage_chunk(request, file_chunk_callback, &read);
        if (err != ESP_OK) return err;
        taskYIELD();
    }
    return ESP_OK;
}

static esp_err_t load_file(const preview_request_t *request, const char *name, size_t maximum, uint8_t **data_out, size_t *size_out)
{
    *data_out = NULL;
    *size_out = 0U;
    size_t size = 0U;
    esp_err_t err = read_file_size(request, name, maximum, &size);
    if (err != ESP_OK) return err;
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) return ESP_ERR_NO_MEM;
    err = read_file_chunks(request, name, size, data);
    if (err != ESP_OK) {
        heap_caps_free(data);
        return err;
    }
    *data_out = data;
    *size_out = size;
    return ESP_OK;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static esp_err_t read_scan_crc(const preview_request_t *request, uint32_t *crc_out)
{
    char path[PREVIEW_PATH_SIZE];
    if (!make_path(path, request, "scan-v1.bin")) return ESP_ERR_INVALID_SIZE;
    file_size_read_t size_read = {.path = path, .size = 0L};
    esp_err_t err = storage_chunk(request, file_size_callback, &size_read);
    if (err != ESP_OK) return err;
    if ((unsigned long)size_read.size > SCAN_FILE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;

    uint8_t buffer[PREVIEW_IO_CHUNK];
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t offset = 0U; offset < (size_t)size_read.size; offset += sizeof(buffer)) {
        if (!request_is_current(request)) return ESP_ERR_INVALID_STATE;
        const size_t remaining = (size_t)size_read.size - offset;
        file_chunk_read_t read = {
            .path = path,
            .offset = offset,
            .requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer),
            .buffer = buffer,
        };
        err = storage_chunk(request, file_chunk_callback, &read);
        if (err != ESP_OK) return err;
        crc = crc32_update(crc, buffer, read.requested);
        taskYIELD();
    }
    *crc_out = crc ^ 0xFFFFFFFFU;
    return ESP_OK;
}

static void set_reason(preview_result_t *result, const char *reason)
{
    if (reason == NULL) reason = "preview unavailable";
    (void)strncpy(result->fallback_reason, reason, sizeof(result->fallback_reason) - 1U);
    result->fallback_reason[sizeof(result->fallback_reason) - 1U] = '\0';
    (void)ui_utf8_truncate(result->fallback_reason, sizeof(result->fallback_reason));
}


static size_t geometry_sensor_count(const object_store_geometry_t *geometry)
{
    size_t count = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (geometry->pods[i].computed) ++count;
    }
    return count;
}

static void load_session(const preview_request_t *request, preview_result_t *result)
{
    result->status = SCAN_PREVIEW_SCREEN_NO_PREVIEW;
    if (!safe_id(request->object_id, sizeof(request->object_id)) || !safe_id(request->session_id, sizeof(request->session_id))) {
        set_reason(result, "invalid session id");
        return;
    }
    if (storage_bulk_admission_is_protected()) {
        set_reason(result, "recording active");
        return;
    }

    geometry_read_t geometry_read = {.request = request, .geometry = &result->geometry};
    if (storage_chunk(request, session_geometry_callback, &geometry_read) == ESP_OK) {
        result->geometry_computed_at_ms = localization_calculate_geometry_computed_at_ms(&result->geometry);
    }

    uint32_t scan_crc = 0U;
    const esp_err_t scan_err = read_scan_crc(request, &scan_crc);
    if (scan_err == ESP_ERR_NOT_FOUND) {
        load_sources(request, result, NULL);
        const size_t sensor_count = geometry_sensor_count(&result->geometry);
        const ui_scan_preview_view_t view = ui_scan_preview_select_view(
            false, false, sensor_count, result->source_count);
        if (view == UI_SCAN_PREVIEW_VIEW_SENSOR_ESTIMATES) {
            result->status = SCAN_PREVIEW_SCREEN_READY;
            result->sensor_estimates_fallback = true;
            set_reason(result, "sensor estimates without scan");
            return;
        }
        result->status = SCAN_PREVIEW_SCREEN_NO_3D_SCAN;
        set_reason(result, "scan missing");
        return;
    }
    char scan_identity[UI_SCAN_PREVIEW_IDENTITY_SIZE] = "";
    if (scan_err == ESP_OK) (void)ui_scan_preview_format_identity(scan_crc, scan_identity, sizeof(scan_identity));
    load_sources(request, result, scan_identity[0] != '\0' ? scan_identity : NULL);
    if (scan_err != ESP_OK) {
        set_reason(result, storage_bulk_admission_is_protected() ? "recording active" : "scan unreadable");
    } else {
        uint8_t *bytes = NULL;
        size_t length = 0U;
        const esp_err_t preview_file_err = load_file(request, "preview-v1.bin", SCAN_PREVIEW_MAX_BYTES, &bytes, &length);
        if (preview_file_err == ESP_OK) {
            const scan_preview_error_t parse_error = scan_preview_parse(bytes, length, NULL, &result->preview);
            heap_caps_free(bytes);
            if (parse_error == SCAN_PREVIEW_OK && result->preview.source_scan_crc32 == scan_crc && result->preview.voxel_count > 0U) {
                const size_t order_bytes = (size_t)result->preview.voxel_count * sizeof(*result->draw_order);
                result->draw_order = heap_caps_malloc(order_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (result->draw_order != NULL) {
                    result->status = SCAN_PREVIEW_SCREEN_READY;
                    return;
                }
                scan_preview_free(&result->preview);
                set_reason(result, "preview memory");
            } else {
                const bool stale = parse_error == SCAN_PREVIEW_OK && result->preview.source_scan_crc32 != scan_crc;
                scan_preview_free(&result->preview);
                set_reason(result, stale ? "stale scan CRC" : "invalid preview");
            }
        } else {
            set_reason(result, storage_bulk_admission_is_protected() ? "recording active" :
                       preview_file_err == ESP_ERR_NOT_FOUND ? "preview missing" : "preview unreadable");
        }
    }

    uint8_t *occupancy_bytes = NULL;
    size_t occupancy_length = 0U;
    const esp_err_t occupancy_file_err = load_file(request, "occupancy-v1.bin", SCAN_OCCUPANCY_MAX_BYTES,
                                                    &occupancy_bytes, &occupancy_length);
    if (occupancy_file_err == ESP_OK) {
        const esp_err_t occupancy_err = scan_occupancy_parse(occupancy_bytes, occupancy_length, &result->occupancy);
        heap_caps_free(occupancy_bytes);
        if (occupancy_err == ESP_OK) {
            /* MAX-415: occupancy of a replaced scan must not be shown or supply a registration. */
            if (scan_err == ESP_OK && ui_scan_preview_occupancy_matches_scan(result->occupancy.frame_id, scan_crc)) {
                result->status = SCAN_PREVIEW_SCREEN_READY;
                return;
            }
            scan_occupancy_free(&result->occupancy);
            if (scan_err == ESP_OK) set_reason(result, "stale scan CRC");
        }
    }
    if (storage_bulk_admission_is_protected()) set_reason(result, "recording active");
    else if (result->fallback_reason[0] == '\0') {
        set_reason(result, occupancy_file_err == ESP_ERR_NOT_FOUND ? "no preview or occupancy" : "no valid preview or occupancy");
    }
}

static void release_result(preview_result_t *result)
{
    scan_preview_free(&result->preview);
    scan_occupancy_free(&result->occupancy);
    if (result->draw_order != NULL) heap_caps_free(result->draw_order);
    memset(result, 0, sizeof(*result));
}

static void refresh_now(uint64_t *flush_us_out)
{
    ui_context_t *context = ui_context();
    const int64_t started = esp_timer_get_time();
    if (s_canvas != NULL) lv_obj_invalidate(s_canvas);
    if (context->display != NULL) lv_refr_now(context->display);
    esp_lcd_panel_io_handle_t io = display_io_handle();
    if (io != NULL) (void)esp_lcd_panel_io_tx_param(io, -1, NULL, 0U);
    const int64_t elapsed = esp_timer_get_time() - started;
    *flush_us_out = elapsed > 0 ? (uint64_t)elapsed : 0U;
}

static const char *preview_reason_text(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') return "";
    if (strcmp(reason, "preview unavailable") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_PREVIEW_UNAVAILABLE);
    if (strcmp(reason, "invalid session id") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_INVALID_SESSION);
    if (strcmp(reason, "recording active") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_RECORDING_ACTIVE);
    if (strcmp(reason, "scan missing") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_SCAN_MISSING);
    if (strcmp(reason, "scan unreadable") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_SCAN_UNREADABLE);
    if (strcmp(reason, "preview memory") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_PREVIEW_MEMORY);
    if (strcmp(reason, "stale scan CRC") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_STALE_CRC);
    if (strcmp(reason, "invalid preview") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_INVALID_PREVIEW);
    if (strcmp(reason, "preview missing") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_PREVIEW_MISSING);
    if (strcmp(reason, "preview unreadable") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_PREVIEW_UNREADABLE);
    if (strcmp(reason, "no preview or occupancy") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_NO_DATA);
    if (strcmp(reason, "sensor estimates without scan") == 0) return ui_tr(UI_I18N_KEY_SCAN_SENSOR_ESTIMATES_FRAME);
    if (strcmp(reason, "no valid preview or occupancy") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_NO_VALID_DATA);
    if (strcmp(reason, "screen allocation failed") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_SCREEN_MEMORY);
    if (strcmp(reason, "canvas memory unavailable") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_CANVAS_MEMORY);
    if (strcmp(reason, "loader unavailable") == 0) return ui_tr(UI_I18N_KEY_SCAN_REASON_LOADER);
    return ui_tr(UI_I18N_KEY_SCAN_REASON_PREVIEW_UNAVAILABLE);
}

static void show_status_locked(scan_preview_screen_status_t status, const char *reason)
{
    if (s_status_label == NULL) return;
    if (s_legend_label != NULL) lv_obj_set_hidden(s_legend_label, true);
    const char *text = status == SCAN_PREVIEW_SCREEN_LOADING ? ui_tr(UI_I18N_KEY_SCAN_LOADING) :
        status == SCAN_PREVIEW_SCREEN_NO_3D_SCAN ? ui_tr(UI_I18N_KEY_SCAN_NO_3D_SCAN) : ui_tr(UI_I18N_KEY_SCAN_NO_PREVIEW);
    lv_label_set_text(s_status_label, text);
    lv_obj_set_hidden(s_status_label, false);
    if (s_info_label != NULL && reason != NULL && reason[0] != '\0') {
        lv_label_set_text(s_info_label, preview_reason_text(reason));
    }
}

static void show_ready_locked(void)
{
    if (s_status_label != NULL) lv_obj_set_hidden(s_status_label, true);
    if (s_info_label == NULL) return;
    if (s_sensor_estimates_fallback) {
        lv_label_set_text(s_info_label, ui_tr(UI_I18N_KEY_SCAN_SENSOR_ESTIMATES_FRAME));
    } else {
        char text[128];
        const char *reason = preview_reason_text(s_state.fallback_reason);
        if (s_state.has_preview) {
            ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_SCAN_VOXELS),
                           (unsigned)s_state.preview.voxel_count,
                           (unsigned)s_state.preview.coarsening_steps,
                           reason[0] != '\0' ? "  " : "", reason);
        } else {
            ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_SCAN_OCCUPANCY),
                     (unsigned)s_state.occupancy.occupied_count, reason);
        }
        lv_label_set_text(s_info_label, text);
    }
    if (s_legend_label != NULL) {
        lv_obj_set_hidden(s_legend_label, s_source_count == 0U);
    }
}

static void add_geometry_overlay(ui_preview3d_scene_t *scene)
{
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT && i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        const object_store_pod_geometry_t *pod = &s_geometry.pods[i];
        if (!pod->computed) continue;
        scene->sensor_valid[i] = true;
        scene->sensors[i] = (ui_preview3d_vec3_t){
            (float)pod->x_mm / 1000.0f,
            (float)pod->y_mm / 1000.0f,
            (float)pod->z_mm / 1000.0f,
        };
    }
}

static void render_ready_locked(void)
{
    const uint32_t generation = s_state.generation;
    if (s_canvas_pixels == NULL || !scan_preview_state_render_begin(&s_state, generation)) return;
    memset(s_canvas_pixels, UI_PREVIEW3D_PX_BACKGROUND, PREVIEW_WIDTH * PREVIEW_HEIGHT);
    ui_preview3d_scene_t scene = {0};
    if (s_state.has_preview) {
        ui_preview3d_scene_set_voxel_preview(&scene, &s_state.preview, s_geometry_computed_at_ms,
                                             s_draw_order, s_state.preview.voxel_count);
    } else if (s_state.has_occupancy) {
        ui_preview3d_scene_set_occupancy(&scene, &s_state.occupancy, s_geometry_computed_at_ms);
    }
    memcpy(scene.sources, s_sources, s_source_count * sizeof(*s_sources));
    scene.source_count = s_source_count;
    add_geometry_overlay(&scene);
    ui_preview3d_stats_t stats;
    const int64_t render_started = esp_timer_get_time();
    ui_preview3d_render(&scene, &s_view,
                        s_sensor_estimates_fallback ? UI_PREVIEW3D_LAYER_SENSORS : UI_PREVIEW3D_LAYER_AUTO,
                        s_canvas_pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT, &stats);
    const int64_t render_elapsed = esp_timer_get_time() - render_started;
    const uint64_t render_us = render_elapsed > 0 ? (uint64_t)render_elapsed : 0U;
    show_ready_locked();
    uint64_t flush_us = 0U;
    refresh_now(&flush_us);
    ESP_LOGI(TAG,
             "PREVIEW_RENDER session=%s voxel_count=%u render_us=%" PRIu64 " flush_us=%" PRIu64 " fallback_reason=%s source_reason=%s",
             s_session_id,
             s_state.has_preview ? s_state.preview.voxel_count : (uint32_t)s_state.occupancy.occupied_count,
             render_us,
             flush_us,
             s_state.fallback_reason[0] != '\0' ? s_state.fallback_reason : "none",
             stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_UNREGISTERED ? "unregistered" :
                 stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_OUTSIDE_GRID ? "outside_grid" :
                 stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_STALE_GEOMETRY ? "stale_geometry" : "none");
    (void)stats;
    scan_preview_state_render_end(&s_state, generation);
    taskYIELD();
}

static void render_current_locked(void)
{
    if (!s_state.open) return;
    if (s_state.status == SCAN_PREVIEW_SCREEN_READY) {
        render_ready_locked();
        return;
    }
    show_status_locked(s_state.status, s_state.fallback_reason);
    uint64_t flush_us = 0U;
    refresh_now(&flush_us);
    (void)flush_us;
}

static void accept_result_locked(const preview_request_t *request, preview_result_t *result)
{
    if (!request_is_current(request)) {
        (void)scan_preview_state_complete(&s_state, request->generation, result->status,
                                          &result->preview, &result->occupancy, result->fallback_reason);
        if (result->draw_order != NULL) heap_caps_free(result->draw_order);
        result->draw_order = NULL;
        return;
    }
    if (ui_context()->screen != UI_SCREEN_SCAN_PREVIEW) {
        release_result(result);
        return;
    }
    s_geometry = result->geometry;
    s_geometry_computed_at_ms = result->geometry_computed_at_ms;
    const bool accepted = scan_preview_state_complete(&s_state, request->generation, result->status,
                                                       &result->preview, &result->occupancy,
                                                       result->fallback_reason);
    if (!accepted) {
        if (result->draw_order != NULL) heap_caps_free(result->draw_order);
        result->draw_order = NULL;
        return;
    }
    s_draw_order = result->draw_order;
    result->draw_order = NULL;
    s_source_count = result->source_count;
    memcpy(s_sources, result->sources, s_source_count * sizeof(*s_sources));
    s_sensor_estimates_fallback = result->sensor_estimates_fallback;
    /* scan_preview_state normally requires an artifact payload for READY. A
     * sensor-frame view owns only geometry and sources, so promote this local
     * state after the shared loader has consumed the empty payload. */
    if (s_sensor_estimates_fallback && s_state.status == SCAN_PREVIEW_SCREEN_NO_PREVIEW) {
        s_state.status = SCAN_PREVIEW_SCREEN_READY;
    }
    render_current_locked();
}

static void preview_loader_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            preview_request_t request;
            bool have_request;
            portENTER_CRITICAL(&s_request_mux);
            have_request = s_request_pending;
            request = s_pending_request;
            s_request_pending = false;
            portEXIT_CRITICAL(&s_request_mux);
            if (!have_request) break;

            preview_result_t result = {0};
            load_session(&request, &result);
            if (!request_is_current(&request)) {
                release_result(&result);
                continue;
            }
            if (!lvgl_port_lock(0)) {
                release_result(&result);
                continue;
            }
            accept_result_locked(&request, &result);
            lvgl_port_unlock();
            release_result(&result);
        }
    }
}

static void request_load(const preview_request_t *request)
{
    portENTER_CRITICAL(&s_request_mux);
    s_pending_request = *request;
    s_request_pending = true;
    portEXIT_CRITICAL(&s_request_mux);
    if (s_worker_task != NULL) xTaskNotifyGive(s_worker_task);
}

static void setup_palette(lv_obj_t *canvas)
{
    static const uint32_t colors[UI_PREVIEW3D_PX_COUNT] = {
        UI_COLOR_BACKGROUND, UI_COLOR_TEXT, UI_COLOR_MUTED, UI_COLOR_GOOD,
        UI_COLOR_ACCENT, UI_COLOR_WARNING, UI_COLOR_ERROR,
        0xE6EEF8, 0x69B9D0, 0x4081A5,
        0xF472B6, 0xA78BFA, 0xFACC15,
    };
    for (uint8_t i = 0U; i < UI_PREVIEW3D_PX_COUNT; ++i) {
        lv_canvas_set_palette(canvas, i, lv_color_to_32(lv_color_hex(colors[i]), LV_OPA_COVER));
    }
}

void ui_scan_preview_open_locked(const char *object_id, const char *session_id)
{
    s_from_workflow = false;
    memset(s_target_object_id, 0, sizeof(s_target_object_id));
    memset(s_target_session_id, 0, sizeof(s_target_session_id));
    if (object_id != NULL) (void)strncpy(s_target_object_id, object_id, sizeof(s_target_object_id) - 1U);
    if (session_id != NULL) (void)strncpy(s_target_session_id, session_id, sizeof(s_target_session_id) - 1U);
    ui_screen_open_locked(UI_SCREEN_SCAN_PREVIEW);
}

void ui_scan_preview_open_from_workflow_locked(const char *object_id, const char *session_id)
{
    s_from_workflow = true;
    memset(s_target_object_id, 0, sizeof(s_target_object_id));
    memset(s_target_session_id, 0, sizeof(s_target_session_id));
    if (object_id != NULL) (void)strncpy(s_target_object_id, object_id, sizeof(s_target_object_id) - 1U);
    if (session_id != NULL) (void)strncpy(s_target_session_id, session_id, sizeof(s_target_session_id) - 1U);
    ui_screen_open_locked(UI_SCREEN_SCAN_PREVIEW);
}

void ui_scan_preview_screen_open_locked(void)
{
    if (!s_state_initialized) {
        scan_preview_state_init(&s_state);
        s_state_initialized = true;
    }
    const uint32_t generation = scan_preview_state_open(&s_state);
    atomic_store_explicit(&s_active_generation, generation, memory_order_release);
    s_session_id[0] = '\0';
    s_source_count = 0U;
    s_sensor_estimates_fallback = false;
    s_workflow_recording = false;
    s_stop_armed = false;
    s_stop_armed_until_ms = 0U;
    s_stop_pending = false;
    s_stop_seq = 0U;
    s_stop_notice_key = UI_I18N_KEY_COUNT;
    s_stop_notice_until_ms = 0U;
    s_workflow_status_ms = 0U;
    if (s_from_workflow) {
        workflow_snapshot_t snapshot;
        s_workflow_recording = workflow_service_try_get_snapshot(&snapshot) == ESP_OK &&
            snapshot.step == WORKFLOW_STEP_RECORDING;
    }
    copy_string(s_session_id, sizeof(s_session_id), s_target_session_id);
    ui_preview3d_view_preset(&s_view, UI_PREVIEW3D_PRESET_ISO);

    lv_obj_t *screen = ui_create_screen_base_locked(ui_tr(UI_I18N_KEY_SCAN_TITLE), ui_tr(UI_I18N_KEY_SCAN_FOOTER));
    if (screen == NULL) {
        (void)scan_preview_state_complete(&s_state, generation, SCAN_PREVIEW_SCREEN_NO_PREVIEW, NULL, NULL, "screen allocation failed");
        return;
    }
    s_canvas = lv_canvas_create(screen);
    if (s_canvas != NULL) {
        lv_obj_set_pos(s_canvas, 0, 34);
        lv_obj_set_size(s_canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        s_canvas_storage = heap_caps_calloc(1U, LV_DRAW_BUF_SIZE(PREVIEW_WIDTH, PREVIEW_HEIGHT, LV_COLOR_FORMAT_I8),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas_storage != NULL) {
            lv_canvas_set_buffer(s_canvas, s_canvas_storage, PREVIEW_WIDTH, PREVIEW_HEIGHT, LV_COLOR_FORMAT_I8);
            lv_obj_t *canvas_obj = s_canvas;
            setup_palette(canvas_obj);
            lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(canvas_obj);
            s_canvas_pixels = draw_buffer != NULL ? lv_draw_buf_goto_xy(draw_buffer, 0U, 0U) : NULL;
        }
    }
    s_status_label = ui_create_label_locked(screen, 10, 106, 300, ui_tr(UI_I18N_KEY_SCAN_LOADING), UI_COLOR_TEXT);
    if (s_status_label != NULL) lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    s_info_label = ui_create_label_locked(screen, 8, 38, 304, "", UI_COLOR_TEXT);
    s_legend_label = ui_create_label_locked(screen, 8, 195, 304, ui_tr(UI_I18N_KEY_SCAN_METHOD_ESTIMATES), UI_COLOR_TEXT);
    if (s_legend_label != NULL) {
        lv_obj_set_style_bg_color(s_legend_label, lv_color_hex(UI_COLOR_HEADER), 0);
        lv_obj_set_style_bg_opa(s_legend_label, LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(s_legend_label, 3, 0);
        lv_obj_set_hidden(s_legend_label, true);
    }
    if (s_info_label != NULL) {
        lv_obj_set_style_bg_color(s_info_label, lv_color_hex(UI_COLOR_HEADER), 0);
        lv_obj_set_style_bg_opa(s_info_label, LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(s_info_label, 3, 0);
    }
    if (s_from_workflow && s_workflow_recording) {
        preview_set_footer(UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_HINT);
    }

    preview_request_t request = {.generation = generation};
    copy_string(request.object_id, sizeof(request.object_id), s_target_object_id);
    copy_string(request.session_id, sizeof(request.session_id), s_target_session_id);
    if (s_canvas_pixels == NULL) {
        (void)scan_preview_state_complete(&s_state, generation, SCAN_PREVIEW_SCREEN_NO_PREVIEW, NULL, NULL, "canvas memory unavailable");
        show_status_locked(s_state.status, s_state.fallback_reason);
        return;
    }
    if (s_worker_task == NULL) {
        const BaseType_t created = ui_scan_preview_task_create(
            preview_loader_task,
            "scan-preview",
            APP_TASK_STACK_OBJECT_RECOVERY,
            NULL,
            APP_TASK_PRIORITY_OBJECT_RECOVERY,
            &s_worker_task,
            APP_TASK_CORE_SERVICES
        );
        if (created != pdPASS) {
            (void)scan_preview_state_complete(&s_state, generation, SCAN_PREVIEW_SCREEN_NO_PREVIEW, NULL, NULL, "loader unavailable");
            show_status_locked(s_state.status, s_state.fallback_reason);
            return;
        }
    }
    request_load(&request);
}

void ui_scan_preview_close_locked(void)
{
    if (!s_state_initialized) return;
    s_source_count = 0U;
    scan_preview_state_close(&s_state);
    atomic_store_explicit(&s_active_generation, s_state.generation, memory_order_release);
}

void ui_scan_preview_release_locked(void)
{
    if (s_canvas_storage != NULL) heap_caps_free(s_canvas_storage);
    if (s_draw_order != NULL) heap_caps_free(s_draw_order);
    s_canvas_storage = NULL;
    s_canvas_pixels = NULL;
    s_canvas = NULL;
    s_status_label = NULL;
    s_info_label = NULL;
    s_legend_label = NULL;
    s_draw_order = NULL;
    s_session_id[0] = '\0';
    memset(&s_geometry, 0, sizeof(s_geometry));
    s_geometry_computed_at_ms = 0U;
}

void ui_scan_preview_render_locked(void)
{
    render_current_locked();
}

void ui_scan_preview_refresh_locked(void)
{
    if (!s_from_workflow) return;
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    if (s_workflow_status_ms != 0U && now_ms - s_workflow_status_ms < 250U) return;
    s_workflow_status_ms = now_ms;
    workflow_snapshot_t snapshot;
    if (workflow_service_try_get_snapshot(&snapshot) != ESP_OK) return;
    s_workflow_recording = snapshot.step == WORKFLOW_STEP_RECORDING;
    if (s_stop_pending && snapshot.last_async.seq >= s_stop_seq) {
        s_stop_pending = false;
        if (snapshot.last_async.result != WORKFLOW_COMMAND_RESULT_OK) {
            s_stop_notice_key = UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_ERROR;
            s_stop_notice_until_ms = now_ms + 2000U;
        }
    }
    if (s_stop_armed && now_ms >= s_stop_armed_until_ms) s_stop_armed = false;
    if (s_stop_pending) {
        preview_set_footer(UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_PENDING);
    } else if (s_stop_notice_key != UI_I18N_KEY_COUNT && now_ms < s_stop_notice_until_ms) {
        preview_set_footer(s_stop_notice_key);
    } else if (s_workflow_recording) {
        s_stop_notice_key = UI_I18N_KEY_COUNT;
        preview_set_footer(s_stop_armed
            ? UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_CONFIRM
            : UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_HINT);
    } else {
        s_stop_notice_key = UI_I18N_KEY_COUNT;
        preview_set_footer(UI_I18N_KEY_SCAN_FOOTER);
    }
}

void ui_scan_preview_handle_key_locked(keypad_key_t key)
{
    if (key == KEYPAD_KEY_LEFT) {
        if (s_from_workflow) ui_screen_open_locked(UI_SCREEN_WORKFLOW);
        else ui_open_home_locked();
        return;
    }
    if (key == KEYPAD_KEY_OK && s_from_workflow && s_workflow_recording) {
        if (s_stop_pending) return;
        const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        if (!ui_workflow_model_preview_ok(true, s_stop_armed, now_ms, &s_stop_armed_until_ms)) {
            s_stop_armed = true;
            preview_set_footer(UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_CONFIRM);
            return;
        }
        s_stop_armed = false;
        workflow_snapshot_t snapshot;
        if (workflow_service_try_get_snapshot(&snapshot) != ESP_OK ||
            snapshot.step != WORKFLOW_STEP_RECORDING) {
            s_stop_notice_key = UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_ERROR;
            s_stop_notice_until_ms = now_ms + 2000U;
            preview_set_footer(s_stop_notice_key);
            return;
        }
        uint32_t seq = 0U;
        const esp_err_t err = workflow_service_command_async(
            WORKFLOW_COMMAND_STOP_RECORDING, true, snapshot.generation, &seq);
        if (err == ESP_OK) {
            s_stop_pending = true;
            s_stop_seq = seq;
            preview_set_footer(UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_PENDING);
        } else {
            s_stop_notice_key = err == ESP_ERR_NO_MEM
                ? UI_I18N_KEY_TFT_WORKFLOW_NOTICE_BUSY
                : UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_ERROR;
            s_stop_notice_until_ms = now_ms + 2000U;
            preview_set_footer(s_stop_notice_key);
        }
        return;
    }
    if (s_state.status != SCAN_PREVIEW_SCREEN_READY) return;
    ui_preview3d_key_t preview_key;
    switch (key) {
        case KEYPAD_KEY_UP: preview_key = UI_PREVIEW3D_KEY_UP; break;
        case KEYPAD_KEY_DOWN: preview_key = UI_PREVIEW3D_KEY_DOWN; break;
        case KEYPAD_KEY_RIGHT: preview_key = UI_PREVIEW3D_KEY_RIGHT; break;
        case KEYPAD_KEY_OK: preview_key = UI_PREVIEW3D_KEY_OK; break;
        default: return;
    }
    if (ui_preview3d_view_apply_key(&s_view, preview_key)) render_current_locked();
}
