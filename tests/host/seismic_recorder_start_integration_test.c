#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(SEISMIC_RECORDER_GATE_BYPASS_TEST)
#include <stdarg.h>
#endif

#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "runtime_observability.h"
#include "device_status.h"
#include "seismic_recorder.h"
#include "seismic_acquisition.h"
#include "seismic_recorder_internal.h"
#include "nrf_cs_authority.h"
#include "nrf_cs_cycle_coordinator.h"
#include "nrf_geometry_solver.h"
#include "nrf_topology_service.h"
#include "object_store.h"
#include "storage_manager.h"

static seismic_acquisition_snapshot_t fake_acquisition;
static FILE *fake_file;
static nrf_topology_snapshot_t fake_topology;
static nrf_topology_leveling_summary_t fake_leveling;
static nrf_cs_cycle_snapshot_t fake_cycle;
static nrf_topology_ranging_result_t fake_frozen_ranging;
static bool fake_reusable_ranging_available;
static unsigned fake_cs_cycle_requests;
static bool fake_radio_on_demand;
static unsigned fake_autonomous_requests;
static unsigned fake_progress_calls;
static esp_err_t fake_autonomous_start_result;
static bool fake_progress_never_completes;
static bool fake_progress_changes_identity;
static unsigned fake_progress_done_after = 1U;
static nrf_remote_sensor_cache_t fake_sensor_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static device_status_snapshot_t fake_device_status;
static object_store_geometry_t last_geometry;
static bool last_geometry_set;
static object_store_cs_pairs_t last_cs_pairs;
static bool last_cs_pairs_set;
static bool fail_cs_pairs_write;
static char persisted_session_json[8192];
static unsigned metadata_write_calls;
static char last_metadata_status[24];
static TaskFunction_t fake_recorder_task;
static bool final_metadata_written;
static bool fake_health_deadline_armed;
static unsigned complete_metadata_writes_after_deadline;
static seismic_frame_t fake_ring_frames[4];
static uint64_t fake_ring_source_indices[4];
static size_t fake_ring_frame_count;
static uint64_t fake_ring_total_frames;
static uint64_t fake_ring_dropped_frames;
static bool fake_ring_batch_pending;
#if defined(SEISMIC_RECORDER_GATE_BYPASS_TEST)
static unsigned recording_gate_bypass_log_count;
static char recording_gate_bypass_log_line[192];

void seismic_recorder_gate_bypass_test_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    if (format == NULL || strstr(format, "RECORDING_GATE_BYPASS") == NULL) {
        return;
    }
    ++recording_gate_bypass_log_count;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(recording_gate_bypass_log_line,
        sizeof(recording_gate_bypass_log_line), format, args);
    va_end(args);
}
#endif
static jmp_buf worker_finished;

typedef struct { unsigned held; } fake_mutex_t;
typedef struct { EventBits_t bits; } fake_event_group_t;
static bool s_force_event_wait_timeout;
static TickType_t s_last_event_wait_timeout;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(fake_mutex_t)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) { (void)timeout; return semaphore != NULL ? pdTRUE : pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { return semaphore != NULL ? pdTRUE : pdFALSE; }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }
EventGroupHandle_t xEventGroupCreate(void) { return calloc(1, sizeof(fake_event_group_t)); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits |= bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits &= ~bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit, BaseType_t wait_for_all, TickType_t timeout) { (void)clear_on_exit; (void)wait_for_all; s_last_event_wait_timeout = timeout; return s_force_event_wait_timeout ? 0U : ((fake_event_group_t *)group)->bits & bits; }
void vEventGroupDelete(EventGroupHandle_t group) { free(group); }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned stack_size, void *argument, unsigned priority, TaskHandle_t *task_handle, int core_id) { (void)name; (void)stack_size; (void)argument; (void)priority; (void)core_id; fake_recorder_task = task; *task_handle = (TaskHandle_t)1; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; if (final_metadata_written) longjmp(worker_finished, 1); }
void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress) { (void)task; (void)progress; }
#if !defined(SEISMIC_RECORDER_HEALTH_PHASE_STUB_EXTERNAL)
void runtime_observability_set_health_phase(runtime_observability_task_t task, runtime_health_phase_t phase) { (void)task; (void)phase; }
#endif
runtime_health_result_t runtime_observability_get_health_result(runtime_observability_task_t task) { (void)task; return fake_health_deadline_armed ? RUNTIME_HEALTH_DEADLINE_EXCEEDED : RUNTIME_HEALTH_OK; }
int64_t esp_timer_get_time(void) { return 1000000; }

bool storage_manager_is_ready(void) { return true; }
esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size) {
    (void)path;
    if (persisted_session_json[0] == '\0') return ESP_ERR_NOT_FOUND;
    const size_t length = strlen(persisted_session_json) + 1U;
    if (length > buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, persisted_session_json, length);
    return ESP_OK;
}
esp_err_t storage_manager_read_text_known_present(const char *path, char *buffer, size_t buffer_size) {
    return storage_manager_read_text(path, buffer, buffer_size);
}
esp_err_t storage_manager_atomic_write_text(const char *path, const char *text) {
    (void)path;
    if (text == NULL || strlen(text) + 1U > sizeof(persisted_session_json)) return ESP_ERR_INVALID_SIZE;
    snprintf(persisted_session_json, sizeof(persisted_session_json), "%s", text);
    ++metadata_write_calls;
    const char *status = strstr(text, "\"status\":\"");
    assert(status != NULL);
    status += strlen("\"status\":\"");
    const char *status_end = strchr(status, '"');
    assert(status_end != NULL);
    const size_t status_length = (size_t)(status_end - status);
    assert(status_length < sizeof(last_metadata_status));
    memcpy(last_metadata_status, status, status_length);
    last_metadata_status[status_length] = '\0';
    if (fake_health_deadline_armed && strcmp(last_metadata_status, "complete") == 0) {
        ++complete_metadata_writes_after_deadline;
    }
    if (strcmp(last_metadata_status, "complete") == 0 || strcmp(last_metadata_status, "error") == 0) {
        final_metadata_written = true;
    }
    return ESP_OK;
}
esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile) {
    (void)object_id;
    memset(profile, 0, sizeof(*profile));
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) profile->cable_lengths_m[i] = 5.0f;
    /* MAX-166: simulate real object_store persistence -- a fetch only sees
     * geometry that a *prior* object_store_set_geometry() call already
     * committed. If seismic_recorder_start() fetched the profile before
     * running the geometry solve, this would return the all-zero default
     * and the ordering regression this test guards against would go
     * unnoticed. */
    if (last_geometry_set) {
        profile->geometry = last_geometry;
    }
    if (last_cs_pairs_set) {
        profile->cs_pairs = last_cs_pairs;
    }
    return ESP_OK;
}
esp_err_t object_store_set_geometry(const char *object_id, const object_store_geometry_t *geometry) { (void)object_id; last_geometry = *geometry; last_geometry_set = true; return ESP_OK; }
esp_err_t object_store_set_cs_pairs(const char *object_id, const object_store_cs_pairs_t *cs_pairs) { (void)object_id; if (fail_cs_pairs_write) return ESP_FAIL; last_cs_pairs = *cs_pairs; last_cs_pairs_set = true; return ESP_OK; }
esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot) { *snapshot = fake_acquisition; return ESP_OK; }

esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary) { *summary = fake_leveling; return ESP_OK; }
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy) { (void)policy; ++fake_cs_cycle_requests; return fake_radio_on_demand ? ESP_FAIL : ESP_OK; }
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot) { *snapshot = fake_cycle; return ESP_OK; }
uint32_t nrf_cs_cycle_coordinator_budget_ms(const nrf_cs_policy_t *policy, size_t executions, size_t nodes)
{
    assert(policy->full_rotation && executions == NRF_CS_MAX_EXECUTIONS && nodes == NRF_CS_MAX_NODES);
    return 92500U;
}
esp_err_t nrf_topology_service_ranging_start_autonomous_limited(const nrf_cs_policy_t *policy, const uint32_t *limits)
{
    assert(limits != NULL && limits[0] == 5000U && limits[3] == 5000U && limits[4] == 0U);
    return nrf_topology_service_ranging_start_autonomous(policy);
}

esp_err_t nrf_topology_service_ranging_start_autonomous(const nrf_cs_policy_t *policy)
{
    assert(policy != NULL && policy->full_rotation);
    ++fake_autonomous_requests;
    fake_progress_calls = 0U;
    return fake_autonomous_start_result;
}
esp_err_t nrf_topology_service_ranging_get_progress(nrf_topology_ranging_progress_t *progress)
{
    memset(progress, 0, sizeof(*progress));
    progress->started_at_ms = fake_progress_changes_identity && fake_progress_calls > 0U ? 124U : 123U;
    progress->active = fake_progress_never_completes || fake_progress_calls < fake_progress_done_after;
    ++fake_progress_calls;
    progress->complete = !progress->active;
    progress->success = fake_cycle.result == NRF_CS_AUTHORITY_COMPLETE;
    progress->membership_generation = fake_cycle.membership_generation;
    progress->pair_count = fake_cycle.pair_count;
    memcpy(progress->pairs, fake_cycle.pairs, sizeof(progress->pairs));
    return ESP_OK;
}
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot) { *snapshot = fake_topology; return ESP_OK; }
esp_err_t nrf_topology_service_get_reusable_frozen_ranging_result(
    const nrf_topology_snapshot_t *topology,
    uint64_t now_ms,
    uint64_t max_age_ms,
    nrf_topology_ranging_result_t *result)
{
    assert(topology != NULL && result != NULL);
    assert(now_ms == 1000U);
    assert(max_age_ms == NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS);
    if (!fake_reusable_ranging_available) return ESP_ERR_NOT_FOUND;
    *result = fake_frozen_ranging;
    return ESP_OK;
}
/* Heights the placement step settled, by node id; none unless a test sets them. */
static uint16_t fake_placement_generation;
static bool fake_placement_valid[16];
static int32_t fake_placement_height_mm[16];
bool nrf_topology_service_get_placement_height_mm(nrf_cs_node_id_t node_id, uint16_t generation, int32_t *height_mm)
{
    if (height_mm == NULL || generation == 0U || generation != fake_placement_generation) return false;
    if (node_id >= 16U || !fake_placement_valid[node_id]) return false;
    *height_mm = fake_placement_height_mm[node_id];
    return true;
}
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot) { if (channel >= NRF_TOPOLOGY_REMOTE_COUNT) return ESP_ERR_INVALID_ARG; *snapshot = fake_sensor_cache[channel]; return ESP_OK; }
esp_err_t device_status_get_snapshot(device_status_snapshot_t *snapshot) { if (snapshot == NULL) return ESP_ERR_INVALID_ARG; *snapshot = fake_device_status; return ESP_OK; }

esp_err_t seismic_recorder_fs_create_session(const char *object_id, const seismic_acquisition_snapshot_t *acquisition, uint8_t channels, char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE], FILE **file, uint16_t *format_version) { (void)object_id; (void)channels; if (acquisition == NULL || format_version == NULL) return ESP_ERR_INVALID_ARG; *format_version = acquisition->source_name != NULL && strcmp(acquisition->source_name, "AD7606") == 0 ? SEISMIC_RECORDER_BINARY_VERSION_V2 : SEISMIC_RECORDER_BINARY_VERSION_V1; snprintf(session_id, SEISMIC_RECORDER_SESSION_ID_SIZE, "S000001"); fake_file = tmpfile(); *file = fake_file; return fake_file != NULL ? ESP_OK : ESP_FAIL; }
esp_err_t seismic_buffer_copy_latest(seismic_frame_t *frames, size_t max_frames, size_t *count, seismic_buffer_info_t *info) { (void)frames; (void)max_frames; *count = 0U; memset(info, 0, sizeof(*info)); info->total_frames = fake_ring_total_frames; info->buffered_frames = fake_ring_frame_count; return ESP_OK; }
esp_err_t seismic_buffer_copy_since(uint64_t *cursor, seismic_frame_t *frames, size_t max_frames, size_t *count, uint64_t *dropped, seismic_buffer_info_t *info) { (void)cursor; (void)frames; (void)max_frames; (void)count; (void)dropped; (void)info; return ESP_ERR_TIMEOUT; }
esp_err_t seismic_buffer_copy_since_with_source_indices(uint64_t *cursor, seismic_frame_t *frames, uint64_t *source_sample_indices, size_t max_frames, size_t *count, uint64_t *dropped, seismic_buffer_info_t *info) {
    if (cursor == NULL || frames == NULL || source_sample_indices == NULL || count == NULL || dropped == NULL || info == NULL) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->total_frames = fake_ring_total_frames;
    info->buffered_frames = fake_ring_frame_count;
    if (!fake_ring_batch_pending) return ESP_ERR_TIMEOUT;
    const size_t copied = fake_ring_frame_count < max_frames ? fake_ring_frame_count : max_frames;
    memcpy(frames, fake_ring_frames, copied * sizeof(frames[0]));
    memcpy(source_sample_indices, fake_ring_source_indices, copied * sizeof(source_sample_indices[0]));
    *count = copied;
    *dropped = fake_ring_dropped_frames;
    *cursor = fake_ring_total_frames;
    fake_ring_batch_pending = false;
    return ESP_OK;
}
esp_err_t seismic_recorder_fs_close(FILE *file) { if (file != NULL) fclose(file); return ESP_OK; }
esp_err_t seismic_recorder_fs_delete_session(const char *object_id, const char *session_id) { (void)object_id; (void)session_id; return ESP_OK; }
esp_err_t seismic_recorder_fs_write_frames(FILE *file, const seismic_frame_t *frames, const uint64_t *sample_indices, size_t count, size_t *written) { (void)file; (void)frames; (void)sample_indices; *written = count; return ESP_OK; }
esp_err_t seismic_recorder_fs_flush(FILE *file, bool sync_to_media) { (void)file; (void)sync_to_media; return ESP_OK; }
esp_err_t seismic_recorder_fs_sync(FILE *file) { (void)file; return ESP_OK; }
esp_err_t seismic_recorder_fs_list_sessions(const char *object_id, seismic_recorder_session_summary_t *sessions, size_t max, size_t *count) { (void)object_id; (void)sessions; (void)max; *count = 0U; return ESP_OK; }
esp_err_t seismic_recorder_fs_build_session_path(const char *object_id, const char *session_id, char *path, size_t path_size) {
    const int written = snprintf(path, path_size, "/objects/%s/sessions/%s", object_id, session_id);
    return written < 0 || written >= (int)path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
esp_err_t seismic_recorder_waveform_read_file(FILE *file, uint64_t size, const seismic_recorder_session_summary_t *summary, uint64_t start, uint64_t count, size_t max_points, seismic_recorder_waveform_point_t *points, size_t capacity, seismic_recorder_waveform_info_t *info) { (void)file; (void)size; (void)summary; (void)start; (void)count; (void)max_points; (void)points; (void)capacity; (void)info; return ESP_ERR_NOT_FOUND; }

static void test_source_gap_summary_is_written(void)
{
    assert(strstr(persisted_session_json, "\"has_gaps\":true") != NULL);
    assert(strstr(persisted_session_json, "\"ring_dropped_frames\":0") != NULL);
    assert(strstr(persisted_session_json, "\"source_gap_samples\":2") != NULL);
}

int main(void)
{
#if defined(SEISMIC_RECORDER_GATE_BYPASS_TEST)
#ifndef SEISMIC_RECORDER_GATE_BYPASS_REMOTE_COUNT
#error "SEISMIC_RECORDER_GATE_BYPASS_REMOTE_COUNT must be set for this test"
#endif
    const uint8_t remote_count = SEISMIC_RECORDER_GATE_BYPASS_REMOTE_COUNT;
    assert(remote_count <= 2U);
    fake_acquisition.running = true;
    fake_acquisition.source_healthy = false;
    fake_acquisition.source_name = "AD7606";
    fake_acquisition.sample_rate_hz = 500U;
    fake_acquisition.latest_valid = true;
    fake_acquisition.samples = 1U;
    fake_acquisition.last_sample_timestamp_us = 999000;
    fake_acquisition.cadence_available = true;
    fake_acquisition.timer_ticks = 100U;
    fake_acquisition.conversion_attempts = 100U;

    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.main.node_id = 0U;
    memset(fake_sensor_cache, 0, sizeof(fake_sensor_cache));
    for (uint8_t channel = 0U; channel < remote_count; ++channel) {
        fake_topology.remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        fake_topology.remotes[channel].node_id = (uint8_t)(channel + 1U);
        fake_topology.remotes[channel].mux_channel = channel;
        fake_sensor_cache[channel].response_valid = true;
        fake_sensor_cache[channel].topology_current = true;
        fake_sensor_cache[channel].sensor.orientation_state =
            NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
    }
    fake_leveling.assigned_count = remote_count;
    fake_leveling.accepted_count = remote_count;

    assert(seismic_recorder_init() == ESP_OK);
    assert(seismic_recorder_start("OBJ_0444") == ESP_OK);
    assert(fake_cs_cycle_requests == 0U);
    assert(!last_geometry_set && !last_cs_pairs_set);
    assert(metadata_write_calls == 1U && strcmp(last_metadata_status, "recording") == 0);
    assert(strstr(persisted_session_json, "\"gate_bypassed\":true") != NULL);
    char assigned_count_json[48];
    snprintf(assigned_count_json, sizeof(assigned_count_json),
        "\"assigned_remote_count\":%u", (unsigned)remote_count);
    assert(strstr(persisted_session_json, assigned_count_json) != NULL);
    assert(recording_gate_bypass_log_count == 1U);
    assert(strstr(recording_gate_bypass_log_line, "reason=remotes_lt_3") != NULL);
    assert(strstr(recording_gate_bypass_log_line, "leveling=1") != NULL);

    /* A duplicate START cannot emit a second bypass record for this session. */
    assert(seismic_recorder_start("OBJ_0444") == ESP_ERR_INVALID_STATE);
    assert(recording_gate_bypass_log_count == 1U);
    return 0;
#else
    memset(&fake_acquisition, 0, sizeof(fake_acquisition));
    fake_acquisition.running = true;
    fake_acquisition.source_healthy = false;
    fake_acquisition.source_name = "AD7606";
    fake_acquisition.sample_rate_hz = 500U;
    fake_acquisition.latest_valid = true;
    fake_acquisition.samples = 1U;
    fake_acquisition.last_sample_timestamp_us = 999000;
    fake_acquisition.cadence_available = true;
    fake_acquisition.timer_ticks = 100U;
    fake_acquisition.conversion_attempts = 100U;
    fake_acquisition.collapsed_periods = 0U;

    /* MAX-165 regression: fewer than 3 assigned REMOTEs must be rejected
     * before any geometry solve is attempted, even though every assigned
     * pod is level -- this is exactly the gap left open by the increment
     * that only landed recording_gate_evaluate() as a tested-but-unwired
     * pure function. Runs first, on a freshly-initialized recorder, so the
     * rejection is observed before s_starting is ever set. */
    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.main.node_id = 0U;
    fake_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[0].node_id = 1U;
    fake_topology.remotes[0].mux_channel = 0U;
    fake_topology.remotes[1].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[1].node_id = 2U;
    fake_topology.remotes[1].mux_channel = 1U;

    fake_leveling.assigned_count = 2U;
    fake_leveling.accepted_count = 2U;

    memset(fake_sensor_cache, 0, sizeof(fake_sensor_cache));
    for (uint8_t channel = 0U; channel < 2U; ++channel) {
        fake_sensor_cache[channel].response_valid = true;
        fake_sensor_cache[channel].topology_current = true;
        fake_sensor_cache[channel].sensor.altitude_mm = 0;
        fake_sensor_cache[channel].sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
    }

    assert(seismic_recorder_init() == ESP_OK);
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_NOT_FOUND);
    assert(!last_geometry_set);
    assert(!last_cs_pairs_set);
    assert(metadata_write_calls == 0U);

    /* MAX-165: three assigned REMOTEs with only two accepted level reports
     * must be rejected as not-level before CS geometry is attempted. Keep the
     * same minimum-capable topology as the success case below so this checks
     * the leveling gate rather than the insufficient-remotes gate. */
    fake_topology.remotes[2].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[2].node_id = 3U;
    fake_topology.remotes[2].mux_channel = 2U;
    fake_leveling.assigned_count = 3U;
    fake_leveling.accepted_count = 2U;

    assert(seismic_recorder_init() == ESP_OK);
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_INVALID_STATE);
    assert(!last_geometry_set);
    assert(!last_cs_pairs_set);
    assert(metadata_write_calls == 0U);

    /* Three assigned, leveled, same-generation reference REMOTE pods on channels
     * 0-2, plus MAIN -- the MAX-165 minimum ("exactly 3 = allowed, planar/2D
     * only") rather than MAX-118's older, now-superseded 1-REMOTE minimum.
     * Reuses the exact mirror-disambiguation 3-4-5-triangle mesh from
     * nrf_geometry_solver_test.c's test_trilateration_with_mirror_disambiguation
     * (MAIN=(0,0), A=(300,0) via a single reference, B=(150,200) and
     * C=(150,-200) distinguished only via their own 400mm pair) so the
     * geometry math here is known-correct, not re-derived. */
    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.generation = 143U;
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.main.node_id = 0U;
    fake_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[0].node_id = 1U;
    fake_topology.remotes[0].mux_channel = 0U;
    fake_topology.remotes[0].hardware_id_length = 2U;
    fake_topology.remotes[0].hardware_id[0] = 0xA0U;
    fake_topology.remotes[0].hardware_id[1] = 0x10U;
    fake_topology.remotes[1].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[1].node_id = 2U;
    fake_topology.remotes[1].mux_channel = 1U;
    fake_topology.remotes[1].hardware_id_length = 2U;
    fake_topology.remotes[1].hardware_id[0] = 0xA1U;
    fake_topology.remotes[1].hardware_id[1] = 0x11U;
    fake_topology.remotes[2].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[2].node_id = 3U;
    fake_topology.remotes[2].mux_channel = 2U;
    fake_topology.remotes[2].hardware_id_length = 2U;
    fake_topology.remotes[2].hardware_id[0] = 0xA2U;
    fake_topology.remotes[2].hardware_id[1] = 0x12U;

    fake_leveling.assigned_count = 3U;
    fake_leveling.accepted_count = 3U;

    memset(fake_sensor_cache, 0, sizeof(fake_sensor_cache));
    memset(&fake_device_status, 0, sizeof(fake_device_status));
    fake_device_status.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    fake_device_status.main.height_reference.generation = 7U;
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        fake_sensor_cache[channel].response_valid = true;
        fake_sensor_cache[channel].topology_current = true;
        fake_sensor_cache[channel].sensor.altitude_mm = 0;
        fake_sensor_cache[channel].sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
        fake_sensor_cache[channel].sensor.reference_generation = 7U;
        fake_sensor_cache[channel].sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
        fake_sensor_cache[channel].sensor.sample_complete_uptime_ms = 980U + channel;
        fake_sensor_cache[channel].esp_receipt_uptime_ms = 990U + channel;
    }

    memset(&fake_cycle, 0, sizeof(fake_cycle));
    fake_cycle.active = false;
    fake_cycle.complete = true;
    fake_cycle.result = NRF_CS_AUTHORITY_COMPLETE;
    fake_cycle.membership_generation = 52U;
    fake_cycle.pair_count = 6U;
    fake_cycle.pairs[0] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID,
        .last_failure = NRF_CS_FAILURE_TIMEOUT, .distance_mm = 300,
        .provider_quality_flags = 3U, .scheduler_quality_flags = NRF_CS_QUALITY_HAD_RETRY,
    };
    fake_cycle.pairs[1] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[2] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[3] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[4] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[5] = (nrf_cs_pair_result_t){ .node_a = 2U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 400 };

    /* Legacy per-step CS cannot reach the reflector with on-demand radios. */
    fake_radio_on_demand = true;

    /* MAX-166 G1: the live object already has a different pair set, and its
     * best-effort update fails. The session must still freeze this cycle. */
    memset(&last_cs_pairs, 0, sizeof(last_cs_pairs));
    last_cs_pairs.pair_count = 1U;
    last_cs_pairs.pairs[0] = (object_store_cs_pair_t) {
        .node_a = 0U, .node_b = 1U, .valid = true, .distance_mm = 7777,
    };
    last_cs_pairs_set = true;
    fail_cs_pairs_write = true;

    assert(seismic_recorder_init() == ESP_OK);
    fake_autonomous_start_result = ESP_ERR_INVALID_STATE;
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_INVALID_STATE);
    assert(!last_geometry_set && metadata_write_calls == 0U);
    fake_autonomous_start_result = ESP_OK;
    fake_cycle.result = NRF_CS_AUTHORITY_ERR_STATE;
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_INVALID_STATE);
    assert(!last_geometry_set && metadata_write_calls == 0U);
    fake_cycle.result = NRF_CS_AUTHORITY_COMPLETE;
    fake_progress_changes_identity = true;
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_INVALID_STATE);
    fake_progress_changes_identity = false;
    fake_progress_never_completes = true;
    assert(seismic_recorder_start("OBJ_0005") == ESP_ERR_TIMEOUT);
    assert(fake_progress_calls > 90000U / 50U);
    assert(!last_geometry_set && metadata_write_calls == 0U);
    fake_progress_never_completes = false;
    fake_progress_done_after = 1000U; /* A valid cycle can exceed the former 45 s limit. */
    /*
     * The sensors read 500, 480 and -300 mm against MAIN at this instant (a wandering barometer), but the
     * placement settled all three at the same height as MAIN for MAIN reference generation 7. The geometry
     * must use the settled one: planar x/y below only come out right when Z is 0.
     */
    fake_sensor_cache[0].sensor.relative_z_mm = 500;
    fake_sensor_cache[1].sensor.relative_z_mm = 480;
    fake_sensor_cache[2].sensor.relative_z_mm = -300;
    fake_placement_generation = 7U;
    fake_placement_valid[1] = fake_placement_valid[2] = fake_placement_valid[3] = true;
    fake_placement_height_mm[1] = fake_placement_height_mm[2] = fake_placement_height_mm[3] = 0;
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);
    assert(fake_autonomous_requests == 5U && fake_cs_cycle_requests == 0U);
    assert(fake_progress_calls == 1001U);
    assert(last_geometry_set);
    assert(!last_geometry.anchored);
    assert(last_geometry.pods[0].computed && last_geometry.pods[0].leveled);
    assert(last_geometry.pods[0].x_mm == 300 && last_geometry.pods[0].y_mm == 0);
    assert(last_geometry.pods[1].computed && last_geometry.pods[1].leveled);
    assert(abs(last_geometry.pods[1].x_mm - 150) <= 2 && abs(last_geometry.pods[1].y_mm - 200) <= 2);
    assert(last_geometry.pods[2].computed && last_geometry.pods[2].leveled);
    /* The mirror-disambiguated point, not (150,200) again. */
    assert(abs(last_geometry.pods[2].x_mm - 150) <= 2 && abs(last_geometry.pods[2].y_mm - (-200)) <= 2);
    /* Z is the settled height, not the reading of the instant that the sensor cache happens to hold. */
    assert(last_geometry.pods[0].z_mm == 0 && last_geometry.pods[1].z_mm == 0 && last_geometry.pods[2].z_mm == 0);

    /* MAX-166: session.json must freeze the geometry this recording solved,
     * not a stale pre-solve fetch. */
    object_store_geometry_t written_geometry;
    const esp_err_t geometry_read_result = seismic_recorder_read_session_geometry("OBJ_0005", "S000001", &written_geometry);
    assert(geometry_read_result == ESP_OK);
    assert(written_geometry.pods[0].computed);
    assert(written_geometry.pods[0].leveled);
    assert(written_geometry.pods[0].x_mm == 300);
    assert(metadata_write_calls == 1U && strcmp(last_metadata_status, "recording") == 0);
    seismic_recorder_session_participation_t start_participation;
    assert(seismic_recorder_read_session_participation("OBJ_0005", "S000001", &start_participation) == ESP_OK);
    assert(start_participation.pods[0].assigned && start_participation.pods[0].node_id_known);
    assert(start_participation.pods[0].node_id == 1U);
    assert(start_participation.pods[0].hardware_id_length == 2U);
    assert(start_participation.pods[0].hardware_id[0] == 0xA0U && start_participation.pods[0].hardware_id[1] == 0x10U);
    assert(start_participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
    assert(start_participation.pods[1].assigned && start_participation.pods[1].node_id_known);
    assert(start_participation.pods[1].node_id == 2U && start_participation.pods[1].hardware_id_length == 2U);
    assert(start_participation.pods[1].hardware_id[0] == 0xA1U && start_participation.pods[1].hardware_id[1] == 0x11U);
    assert(start_participation.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
    assert(start_participation.pods[2].assigned && start_participation.pods[2].node_id_known);
    assert(start_participation.pods[2].node_id == 3U && start_participation.pods[2].hardware_id_length == 2U);
    assert(start_participation.pods[2].hardware_id[0] == 0xA2U && start_participation.pods[2].hardware_id[1] == 0x12U);
    assert(start_participation.pods[2].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
    assert(start_participation.z_provenance.available);
    assert(start_participation.z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE);
    assert(start_participation.z_provenance.fallback_reason[0] == '\0');
    assert(start_participation.geometry_provenance.available);
    assert(strcmp(start_participation.geometry_provenance.ranging_source, "start_cycle") == 0);
    assert(strcmp(start_participation.geometry_provenance.solver, "nrf_geometry_solver") == 0);
    assert(strcmp(start_participation.geometry_provenance.solver_version, NRF_GEOMETRY_SOLVER_VERSION) == 0);
    assert(start_participation.geometry_provenance.topology_generation == 143U);
    assert(start_participation.geometry_provenance.cs_membership_generation == 52U);
    assert(start_participation.geometry_provenance.main_height_reference_generation == 7U);
    assert(start_participation.geometry_provenance.start_uptime_ms == 1000U);
    assert(strcmp(start_participation.geometry_provenance.setup_id, "1000-143-52-7") == 0);
    assert(start_participation.pods[0].source_sample_uptime_ms == 980U);
    assert(start_participation.pods[0].esp_receipt_uptime_ms == 990U);
    assert(start_participation.pods[0].source_age_at_start_known);
    assert(start_participation.pods[0].source_age_ms_at_start == 10U);
    assert(start_participation.pods[0].height_reference_generation == 7U);
    assert(!start_participation.pods[3].assigned && !start_participation.pods[3].node_id_known);
    assert(start_participation.pods[3].hardware_id_length == 0U);
    assert(start_participation.pods[3].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);

    /* MAX-148: the same "solve first, persist after" ordering must hold for
     * the CS pair evidence behind that solve. */
    assert(last_cs_pairs_set);
    assert(last_cs_pairs.pair_count == 1U);
    assert(last_cs_pairs.pairs[0].distance_mm == 7777);
    object_store_cs_pairs_t written_cs_pairs;
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0005", "S000001", &written_cs_pairs) == ESP_OK);
    assert(written_cs_pairs.pair_count == 6U);
    for (size_t i = 0U; i < fake_cycle.pair_count; ++i) {
        assert(written_cs_pairs.pairs[i].node_a == fake_cycle.pairs[i].node_a);
        assert(written_cs_pairs.pairs[i].node_b == fake_cycle.pairs[i].node_b);
        assert(written_cs_pairs.pairs[i].valid == (fake_cycle.pairs[i].state == NRF_CS_PAIR_VALID));
        assert(written_cs_pairs.pairs[i].distance_mm == fake_cycle.pairs[i].distance_mm);
    }
    assert(written_cs_pairs.pairs[0].distance_mm == 300);
    assert(strcmp(written_cs_pairs.pairs[0].state, "valid") == 0);
    assert(strcmp(written_cs_pairs.pairs[0].last_failure, "timeout") == 0);
    assert(written_cs_pairs.pairs[0].provider_quality_flags == 3U);
    assert(written_cs_pairs.pairs[0].scheduler_quality_flags == NRF_CS_QUALITY_HAD_RETRY);

    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_RECORDING);
    assert(strcmp(snapshot.object_id, "OBJ_0005") == 0);
    assert(!snapshot.cadence.available);

    fake_acquisition.timer_ticks = 106U;
    fake_acquisition.conversion_attempts = 104U;
    fake_acquisition.collapsed_periods = 2U;

    /* Live state may move while this recording runs; STOP must rewrite the
     * session from the frozen START copies. */
    object_store_geometry_t moved_geometry = {0};
    moved_geometry.pods[0].computed = true;
    moved_geometry.pods[0].x_mm = 9999;
    assert(object_store_set_geometry("OBJ_0005", &moved_geometry) == ESP_OK);
    object_store_cs_pairs_t moved_pairs = {0};
    moved_pairs.pair_count = 1U;
    moved_pairs.pairs[0].distance_mm = 8888;
    fail_cs_pairs_write = false;
    assert(object_store_set_cs_pairs("OBJ_0005", &moved_pairs) == ESP_OK);
    fake_topology.generation = 701U;
    fake_cycle.membership_generation = 702U;
    fake_device_status.main.height_reference.generation = 703U;
    fake_sensor_cache[0].sensor.reference_generation = 704U;

    fake_ring_frame_count = 4U;
    fake_ring_total_frames = 4U;
    fake_ring_dropped_frames = 0U;
    fake_ring_batch_pending = true;
    fake_ring_source_indices[0] = 100U;
    fake_ring_source_indices[1] = 101U;
    fake_ring_source_indices[2] = 104U;
    fake_ring_source_indices[3] = 105U;

    assert(seismic_recorder_stop() == ESP_OK);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_STOPPING);
    assert(snapshot.cadence.available);
    assert(snapshot.cadence.timer_ticks == 6U);
    assert(snapshot.cadence.conversion_attempts == 4U);
    assert(snapshot.cadence.collapsed_periods == 2U);

    assert(fake_recorder_task != NULL);
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "recorder worker should finish the STOP metadata rewrite");
    }
    assert(final_metadata_written);
    assert(metadata_write_calls == 2U && strcmp(last_metadata_status, "complete") == 0);
    test_source_gap_summary_is_written();
    seismic_recorder_session_participation_t final_participation;
    assert(seismic_recorder_read_session_participation("OBJ_0005", "S000001", &final_participation) == ESP_OK);
    for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
        assert(final_participation.pods[pod].assigned == start_participation.pods[pod].assigned);
        assert(final_participation.pods[pod].node_id_known == start_participation.pods[pod].node_id_known);
        assert(final_participation.pods[pod].node_id == start_participation.pods[pod].node_id);
        assert(final_participation.pods[pod].hardware_id_length == start_participation.pods[pod].hardware_id_length);
        assert(memcmp(final_participation.pods[pod].hardware_id, start_participation.pods[pod].hardware_id, sizeof(start_participation.pods[pod].hardware_id)) == 0);
        assert(final_participation.pods[pod].position_source == start_participation.pods[pod].position_source);
    }
    assert(final_participation.geometry_provenance.topology_generation == 143U);
    assert(final_participation.geometry_provenance.cs_membership_generation == 52U);
    assert(final_participation.geometry_provenance.main_height_reference_generation == 7U);
    assert(strcmp(final_participation.geometry_provenance.ranging_source, "start_cycle") == 0);
    assert(final_participation.pods[0].height_reference_generation == 7U);
    object_store_geometry_t final_geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0005", "S000001", &final_geometry) == ESP_OK);
    assert(final_geometry.pods[0].x_mm == 300);
    object_store_cs_pairs_t final_cs_pairs;
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0005", "S000001", &final_cs_pairs) == ESP_OK);
    assert(final_cs_pairs.pair_count == 6U);
    assert(final_cs_pairs.pairs[0].distance_mm == 300);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running && snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_COMPLETE);
    assert(snapshot.source_gap_samples == 2U);

    fake_ring_frame_count = 0U;
    fake_ring_total_frames = 0U;
    fake_ring_dropped_frames = 0U;
    fake_ring_batch_pending = false;
    fake_frozen_ranging.available = true;
    fake_frozen_ranging.pair_count = fake_cycle.pair_count;
    memcpy(fake_frozen_ranging.pairs, fake_cycle.pairs, sizeof(fake_cycle.pairs));
    fake_frozen_ranging.topology_generation = 143U;
    fake_frozen_ranging.membership_generation = 52U;
    fake_frozen_ranging.timestamp_ms = 999U;
    fake_reusable_ranging_available = true;
    const unsigned cycle_requests_before_reuse = fake_autonomous_requests;
    assert(seismic_recorder_start("OBJ_0006") == ESP_OK);
    assert(fake_autonomous_requests == cycle_requests_before_reuse);
    seismic_recorder_session_participation_t reused_participation;
    assert(seismic_recorder_read_session_participation("OBJ_0006", "S000001", &reused_participation) == ESP_OK);
    assert(strcmp(reused_participation.geometry_provenance.ranging_source, "workflow_ranging") == 0);
    assert(reused_participation.geometry_provenance.topology_generation == 143U);
    assert(reused_participation.geometry_provenance.cs_membership_generation == 52U);
    assert(strcmp(reused_participation.geometry_provenance.setup_id, "1000-143-52-703") == 0);
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0006", "S000001", &written_cs_pairs) == ESP_OK);
    assert(written_cs_pairs.pair_count == 6U && written_cs_pairs.pairs[0].distance_mm == 300);

    assert(seismic_recorder_stop() == ESP_OK);
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "reused-ranging recorder worker should finish the STOP metadata rewrite");
    }
    assert(final_metadata_written && strcmp(last_metadata_status, "complete") == 0);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running && snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_COMPLETE);

    fake_health_deadline_armed = false;
    assert(seismic_recorder_start("OBJ_0007") == ESP_OK);
    assert(strcmp(last_metadata_status, "recording") == 0);
    assert(seismic_recorder_stop() == ESP_OK);
    fake_health_deadline_armed = true;
    complete_metadata_writes_after_deadline = 0U;
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "health timeout should mark the session incomplete and finish safe-stop");
    }
    assert(final_metadata_written && strcmp(last_metadata_status, "error") == 0);
    assert(complete_metadata_writes_after_deadline == 0U);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running && snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_ERROR);
    fake_health_deadline_armed = false;
    /* Reuse an accepted 5-node partial mesh without replacing its missing edge. */
    fake_topology.remotes[3] = fake_topology.remotes[2];
    fake_topology.remotes[3].node_id = 4U;
    fake_topology.remotes[3].mux_channel = 3U;
    fake_topology.remotes[3].hardware_id[0] = 0xA3U;
    fake_leveling.assigned_count = fake_leveling.accepted_count = 4U;
    fake_sensor_cache[3] = fake_sensor_cache[0];
    fake_placement_valid[4] = true;
    fake_placement_height_mm[4] = 0;
    /* An old reusable result can claim success although its full mesh is ill-conditioned. */
    const int32_t collinear_distances[] = {1000,2000,3000,4000,1000,2000,3000,1000,2000,1000};
    fake_frozen_ranging.success = true;
    fake_frozen_ranging.pair_count = 10U;
    size_t collinear_edge = 0U;
    for (uint8_t a = 0U; a < 4U; ++a) {
        for (uint8_t b = a + 1U; b < 5U; ++b) {
            fake_frozen_ranging.pairs[collinear_edge] = (nrf_cs_pair_result_t){.node_a=a,.node_b=b,
                .state=NRF_CS_PAIR_VALID,.distance_mm=collinear_distances[collinear_edge],.confidence=900};
            ++collinear_edge;
        }
    }
    const unsigned before_uncertain = fake_autonomous_requests;
    const unsigned metadata_before_uncertain = metadata_write_calls;
    last_geometry_set = false;
    assert(seismic_recorder_start("OBJ_0009") == ESP_ERR_INVALID_STATE);
    assert(!last_geometry_set && metadata_write_calls == metadata_before_uncertain);
    assert(fake_autonomous_requests == before_uncertain);
    assert(fake_frozen_ranging.pairs[9].distance_mm == 1000);
    const int32_t mirrored_distances[]={2000,4000,1803,3354,2000,1803,1803,3354,1803,0};
    size_t mirror_edge=0U;
    for(uint8_t a=0;a<4;++a)for(uint8_t b=a+1;b<5;++b){
        fake_frozen_ranging.pairs[mirror_edge]=(nrf_cs_pair_result_t){.node_a=a,.node_b=b,
            .state=mirror_edge==9U?NRF_CS_PAIR_RETRY_EXHAUSTED:NRF_CS_PAIR_VALID,
            .distance_mm=mirrored_distances[mirror_edge],.confidence=900,.valid_samples=9U};
        ++mirror_edge;
    }
    const unsigned metadata_before_mirror=metadata_write_calls;
    last_geometry_set=false;
    assert(seismic_recorder_start("OBJ_0009")==ESP_ERR_INVALID_STATE);
    assert(!last_geometry_set && metadata_write_calls==metadata_before_mirror);
    assert(fake_autonomous_requests==before_uncertain);
    assert(fake_frozen_ranging.pairs[3].distance_mm==3354 && fake_frozen_ranging.pairs[9].distance_mm==0);
    const int32_t partial_distances[] = {3000,3000,4243,1803,4243,3000,1803,3000,2500,0};
    fake_frozen_ranging.success = true;
    fake_frozen_ranging.pair_count = 10U;
    size_t edge = 0U;
    for (uint8_t a = 0U; a < 4U; ++a) {
        for (uint8_t b = a + 1U; b < 5U; ++b) {
            fake_frozen_ranging.pairs[edge] = (nrf_cs_pair_result_t){.node_a=a,.node_b=b,
                .state=edge == 9U ? NRF_CS_PAIR_RETRY_EXHAUSTED : NRF_CS_PAIR_VALID,
                .distance_mm=partial_distances[edge],.confidence=900,.valid_samples=9U};
            ++edge;
        }
    }
    fake_frozen_ranging.pairs[0].scheduler_quality_flags = NRF_CS_QUALITY_MANUAL;
    fake_frozen_ranging.pairs[0].automatic_distance_mm = 6000;
    const unsigned before_partial = fake_autonomous_requests;
    assert(seismic_recorder_start("OBJ_0008") == ESP_OK);
    assert(fake_autonomous_requests == before_partial);
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0008", "S000001", &written_cs_pairs) == ESP_OK);
    assert(written_cs_pairs.pair_count == 10U);
    assert(!written_cs_pairs.pairs[9].valid && strcmp(written_cs_pairs.pairs[9].state, "retry_exhausted") == 0);
    assert((written_cs_pairs.pairs[0].scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) != 0U);
    assert(written_cs_pairs.pairs[0].automatic_distance_mm == 6000);
    assert(seismic_recorder_read_session_geometry("OBJ_0008", "S000001", &final_geometry) == ESP_OK);
    for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) assert(final_geometry.pods[pod].computed);
    assert(seismic_recorder_stop() == ESP_OK);
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "partial-mesh recorder worker should finish STOP metadata");
    }
    assert(final_metadata_written && strcmp(last_metadata_status, "complete") == 0);
    /* A co-located first REMOTE must not prevent the other three from defining the frame. */
    const int32_t coincident_distances[] = {0,3000,4000,5000,3000,4000,5000,5000,4000,3000};
    edge = 0U;
    for (uint8_t a = 0U; a < 4U; ++a) {
        for (uint8_t b = a + 1U; b < 5U; ++b) {
            fake_frozen_ranging.pairs[edge] = (nrf_cs_pair_result_t){.node_a=a,.node_b=b,
                .state=NRF_CS_PAIR_VALID,.distance_mm=coincident_distances[edge],.confidence=900,.valid_samples=9U};
            ++edge;
        }
    }
    const unsigned before_coincident = fake_autonomous_requests;
    assert(seismic_recorder_start("OBJ_0009") == ESP_OK);
    assert(fake_autonomous_requests == before_coincident);
    assert(seismic_recorder_read_session_geometry("OBJ_0009", "S000001", &final_geometry) == ESP_OK);
    const int32_t coincident_xy[4][2] = {{0,0},{3000,0},{0,4000},{3000,4000}};
    for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
        assert(final_geometry.pods[pod].computed);
        assert(abs(final_geometry.pods[pod].x_mm - coincident_xy[pod][0]) <= 5);
        assert(abs(final_geometry.pods[pod].y_mm - coincident_xy[pod][1]) <= 5);
    }
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0009", "S000001", &written_cs_pairs) == ESP_OK);
    assert(written_cs_pairs.pair_count == 10U && written_cs_pairs.pairs[0].valid && written_cs_pairs.pairs[0].distance_mm == 0);
    assert(seismic_recorder_stop() == ESP_OK);
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "coincident-mesh recorder worker should finish STOP metadata");
    }
    assert(final_metadata_written && strcmp(last_metadata_status, "complete") == 0);
    return 0;
#endif
}
