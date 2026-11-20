/*
 * The firmware's own source-localization run (localization_calculate_run) compiled to WebAssembly, so a phone can do the
 * calculation on a recording it downloaded and send only the result back to the ESP32. The calculation code is not copied
 * or ported: the same files are built, and this file replaces the nine things the run reaches outside itself (the
 * recording, the frozen geometry, the clock, the run store, the storage guard) with buffers that the page fills.
 *
 * Host-side the same seam is faked by components/localization_calculate/tests/localization_calculate_run_test.c.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "localization_calculate.h"
#include "localization_config.h"
#include "localization_pick_record.h"
#include "localization_run_record.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "seismic_recorder.h"

#define LW_MAX_RECORDS 64U

static seismic_frame_t *s_frames;
static uint64_t *s_source_indices;
static uint8_t *s_gap_before;
static size_t s_frame_count;
static seismic_recorder_session_summary_t s_summary;
static object_store_geometry_t s_geometry;
static int64_t s_now_us = 1000000;

static localization_run_record_t s_records[LW_MAX_RECORDS];
static size_t s_record_count;
static localization_pick_record_t s_pick_record;
static bool s_pick_written;
static localization_calculate_summary_t s_run_summary;
static char *s_result_json;

/* ---- what the run reaches outside itself ------------------------------------------------------------------------ */

int64_t esp_timer_get_time(void) { return s_now_us; }

esp_err_t storage_manager_operation_begin(void) { return ESP_OK; }

void storage_manager_operation_end(void) {}

esp_err_t seismic_recorder_get_session_summary(const char *object_id, const char *session_id,
                                               seismic_recorder_session_summary_t *summary_out)
{
    (void)object_id; (void)session_id;
    if (summary_out == NULL) return ESP_ERR_INVALID_ARG;
    *summary_out = s_summary;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id, const char *session_id, object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source, esp_err_t *registered_status)
{
    (void)object_id; (void)session_id;
    if (geometry == NULL) return ESP_ERR_INVALID_ARG;
    *geometry = s_geometry;
    if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    if (registered_status != NULL) *registered_status = ESP_OK;
    return ESP_OK;
}

/* The tag layout the localization may read beside the geometry: none in this fixture. */
esp_err_t seismic_recorder_read_session_tag_layout(const char *object_id, const char *session_id, seismic_recorder_tag_layout_t *layout)
{
    (void)object_id;
    (void)session_id;
    if (layout != NULL) memset(layout, 0, sizeof(*layout));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t seismic_recorder_read_session_range_ex(
    const char *object_id, const char *session_id, uint64_t start_index, size_t max_frames, seismic_frame_t *frames,
    uint64_t *source_sample_indices, bool *gap_before, size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out, seismic_recorder_raw_window_stats_t *raw_stats_out)
{
    (void)object_id; (void)session_id;
    if (frames == NULL || frame_count_out == NULL || start_index > s_frame_count || max_frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t remaining = s_frame_count - (size_t)start_index;
    const size_t count = remaining < max_frames ? remaining : max_frames;
    memcpy(frames, s_frames + (size_t)start_index, sizeof(*frames) * count);
    for (size_t i = 0U; i < count; ++i) {
        const size_t index = (size_t)start_index + i;
        if (source_sample_indices != NULL) source_sample_indices[i] = s_source_indices[index];
        if (gap_before != NULL) gap_before[i] = s_gap_before[index] != 0U;
    }
    *frame_count_out = count;
    if (raw_stats_out != NULL) {
        memset(raw_stats_out, 0, sizeof(*raw_stats_out));
        raw_stats_out->samples_examined = count;
        for (size_t i = 0U; i < count; ++i) {
            for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
                const float uv = s_frames[(size_t)start_index + i].channel_uv[ch];
                /* int16 rails of the AD7606 at the +-5 V range: the same test the firmware's reader makes on raw codes. */
                if (uv >= 4999847.0f || uv <= -5000000.0f) ++raw_stats_out->rail_sample_counts[ch];
            }
        }
    }
    if (summary_out != NULL) *summary_out = s_summary;
    return ESP_OK;
}

esp_err_t seismic_recorder_scan_session(const char *object_id, const char *session_id, seismic_frame_t *buffer,
                                        size_t capacity, seismic_recorder_scan_callback_t callback, void *context)
{
    (void)object_id; (void)session_id;
    if (buffer == NULL || capacity == 0U || callback == NULL) return ESP_ERR_INVALID_ARG;
    /* Chunks end at every hole so the callback sees the gap flag on its first frame, as the file reader does. */
    for (size_t start = 0U; start < s_frame_count;) {
        size_t count = s_frame_count - start;
        if (count > capacity) count = capacity;
        for (size_t i = 1U; i < count; ++i) {
            if (s_gap_before[start + i] != 0U) { count = i; break; }
        }
        memcpy(buffer, s_frames + start, count * sizeof(*buffer));
        const esp_err_t err = callback(buffer, count, s_source_indices[start], s_gap_before[start] != 0U, context);
        if (err != ESP_OK) return err;
        start += count;
    }
    return ESP_OK;
}

esp_err_t localization_run_store_write(const char *object_id, const char *session_id,
                                       const localization_run_record_t *record)
{
    (void)object_id; (void)session_id;
    if (s_record_count >= LW_MAX_RECORDS) return ESP_ERR_NO_MEM;
    s_records[s_record_count++] = *record;
    return ESP_OK;
}

esp_err_t localization_run_store_write_picks(const char *object_id, const char *session_id,
                                             const localization_pick_record_t *record)
{
    (void)object_id; (void)session_id;
    s_pick_record = *record;
    s_pick_written = true;
    return ESP_OK;
}

/* ---- what the page calls ------------------------------------------------------------------------------------------ */

void *lw_alloc(size_t bytes) { return malloc(bytes); }
void lw_free(void *pointer) { free(pointer); }
size_t lw_frame_size(void) { return sizeof(seismic_frame_t); }
size_t lw_frame_timestamp_offset(void) { return offsetof(seismic_frame_t, timestamp_us); }
size_t lw_frame_channels_offset(void) { return offsetof(seismic_frame_t, channel_uv); }

void lw_reset(void)
{
    free(s_frames); free(s_source_indices); free(s_gap_before); free(s_result_json);
    s_frames = NULL; s_source_indices = NULL; s_gap_before = NULL; s_result_json = NULL;
    s_frame_count = 0U; s_record_count = 0U; s_pick_written = false;
    memset(&s_summary, 0, sizeof(s_summary));
    memset(&s_geometry, 0, sizeof(s_geometry));
    memset(&s_pick_record, 0, sizeof(s_pick_record));
    memset(&s_run_summary, 0, sizeof(s_run_summary));
    s_now_us = 1000000;
}

/* The page allocates the three arrays with lw_alloc and hands them over; lw_reset releases them. */
esp_err_t lw_set_recording(seismic_frame_t *frames, uint64_t *source_indices, uint8_t *gap_before, size_t count,
                           uint32_t sample_rate_hz)
{
    if (frames == NULL || source_indices == NULL || gap_before == NULL || count == 0U || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_frames = frames; s_source_indices = source_indices; s_gap_before = gap_before; s_frame_count = count;
    memset(&s_summary, 0, sizeof(s_summary));
    s_summary.sample_rate_hz = sample_rate_hz;
    s_summary.channel_count = OBJECT_STORE_POD_COUNT;
    s_summary.samples_written = count;
    s_summary.complete = true;
    s_summary.readable = true;
    return ESP_OK;
}

/* The session's frozen geometry exactly as the device stores it (the JSON object of the session). */
esp_err_t lw_set_geometry_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    memset(&s_geometry, 0, sizeof(s_geometry));
    object_store_geometry_from_json(root, &s_geometry);
    cJSON_Delete(root);
    return ESP_OK;
}

/* The calculation settings alone: lets the page tell "bad settings" from "the run failed". */
esp_err_t lw_check_config(const char *config_json)
{
    cJSON *root = cJSON_Parse(config_json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    uint32_t budget_ms = 0U;
    const esp_err_t err = localization_config_from_json(root, &config, &budget_ms);
    cJSON_Delete(root);
    return err;
}

esp_err_t lw_run(const char *object_id, const char *session_id, const char *config_json, uint64_t computed_at_ms)
{
    cJSON *root = cJSON_Parse(config_json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    uint32_t budget_ms = 0U;
    const esp_err_t config_err = localization_config_from_json(root, &config, &budget_ms);
    cJSON_Delete(root);
    if (config_err != ESP_OK) return config_err;
    s_record_count = 0U;
    s_pick_written = false;
    return localization_calculate_run(object_id, session_id, &config, computed_at_ms, &s_run_summary);
}

/* {"picks": <pick record> | null, "runs": [<run record>, ...]} in the schema the device itself stores. */
const char *lw_result_json(void)
{
    free(s_result_json);
    s_result_json = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *runs = cJSON_AddArrayToObject(root, "runs");
    for (size_t i = 0U; i < s_record_count; ++i) {
        char *text = NULL;
        if (localization_run_record_to_json(&s_records[i], &text) != ESP_OK || text == NULL) continue;
        cJSON *parsed = cJSON_Parse(text);
        free(text);
        if (parsed != NULL) cJSON_AddItemToArray(runs, parsed);
    }
    if (s_pick_written) {
        char *text = NULL;
        cJSON *parsed = NULL;
        if (localization_pick_record_to_json(&s_pick_record, &text) == ESP_OK && text != NULL) parsed = cJSON_Parse(text);
        free(text);
        if (parsed != NULL) cJSON_AddItemToObject(root, "picks", parsed); else cJSON_AddNullToObject(root, "picks");
    } else {
        cJSON_AddNullToObject(root, "picks");
    }
    s_result_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s_result_json;
}

size_t lw_record_count(void) { return s_record_count; }
int lw_channel_health_flat(void) { return s_run_summary.channel_health.flat_channel_mask; }
int lw_channel_health_identical(void) { return s_run_summary.channel_health.identical_pair_mask; }
