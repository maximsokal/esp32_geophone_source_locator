#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "runtime_observability.h"
#include "seismic_recorder.h"
#include "seismic_acquisition.h"
#include "seismic_recorder_internal.h"
#include "nrf_cs_authority.h"
#include "nrf_topology_service.h"
#include "object_store.h"
#include "storage_manager.h"

static seismic_acquisition_snapshot_t fake_acquisition;
static FILE *fake_file;
static nrf_topology_snapshot_t fake_topology;
static nrf_topology_leveling_summary_t fake_leveling;
static nrf_cs_cycle_snapshot_t fake_cycle;
static nrf_remote_sensor_cache_t fake_sensor_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static object_store_geometry_t last_geometry;
static bool last_geometry_set;
static object_store_cs_pairs_t last_cs_pairs;
static bool last_cs_pairs_set;
static object_store_profile_t last_written_profile;
static bool last_written_profile_set;

typedef struct { unsigned held; } fake_mutex_t;
typedef struct { EventBits_t bits; } fake_event_group_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(fake_mutex_t)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) { (void)timeout; return semaphore != NULL ? pdTRUE : pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { return semaphore != NULL ? pdTRUE : pdFALSE; }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }
EventGroupHandle_t xEventGroupCreate(void) { return calloc(1, sizeof(fake_event_group_t)); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits |= bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits &= ~bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit, BaseType_t wait_for_all, TickType_t timeout) { (void)clear_on_exit; (void)wait_for_all; (void)timeout; return ((fake_event_group_t *)group)->bits & bits; }
void vEventGroupDelete(EventGroupHandle_t group) { free(group); }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned stack_size, void *argument, unsigned priority, TaskHandle_t *task_handle, int core_id) { (void)task; (void)name; (void)stack_size; (void)argument; (void)priority; (void)core_id; *task_handle = (TaskHandle_t)1; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress) { (void)task; (void)progress; }
int64_t esp_timer_get_time(void) { return 1000000; }

bool storage_manager_is_ready(void) { return true; }
esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile) {
    (void)object_id;
    memset(profile, 0, sizeof(*profile));
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
esp_err_t object_store_set_cs_pairs(const char *object_id, const object_store_cs_pairs_t *cs_pairs) { (void)object_id; last_cs_pairs = *cs_pairs; last_cs_pairs_set = true; return ESP_OK; }
esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot) { *snapshot = fake_acquisition; return ESP_OK; }

esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary) { *summary = fake_leveling; return ESP_OK; }
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy) { (void)policy; return ESP_OK; }
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot) { *snapshot = fake_cycle; return ESP_OK; }
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot) { *snapshot = fake_topology; return ESP_OK; }
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot) { if (channel >= NRF_TOPOLOGY_REMOTE_COUNT) return ESP_ERR_INVALID_ARG; *snapshot = fake_sensor_cache[channel]; return ESP_OK; }

esp_err_t seismic_recorder_fs_create_session(const char *object_id, uint32_t rate, uint8_t channels, char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE], FILE **file) { (void)object_id; (void)rate; (void)channels; snprintf(session_id, SEISMIC_RECORDER_SESSION_ID_SIZE, "S000001"); fake_file = tmpfile(); *file = fake_file; return fake_file != NULL ? ESP_OK : ESP_FAIL; }
esp_err_t seismic_recorder_json_write_metadata(const object_store_profile_t *profile, const seismic_recorder_snapshot_t *snapshot, const char *status) { (void)snapshot; (void)status; last_written_profile = *profile; last_written_profile_set = true; return ESP_OK; }
esp_err_t seismic_buffer_copy_latest(seismic_frame_t *frames, size_t max_frames, size_t *count, seismic_buffer_info_t *info) { (void)frames; (void)max_frames; *count = 0U; memset(info, 0, sizeof(*info)); info->total_frames = 0U; return ESP_OK; }
esp_err_t seismic_buffer_copy_since(uint64_t *cursor, seismic_frame_t *frames, size_t max_frames, size_t *count, uint64_t *dropped, seismic_buffer_info_t *info) { (void)cursor; (void)frames; (void)max_frames; (void)count; (void)dropped; (void)info; return ESP_ERR_TIMEOUT; }
esp_err_t seismic_recorder_fs_close(FILE *file) { if (file != NULL) fclose(file); return ESP_OK; }
esp_err_t seismic_recorder_fs_delete_session(const char *object_id, const char *session_id) { (void)object_id; (void)session_id; return ESP_OK; }
esp_err_t seismic_recorder_fs_write_frames(FILE *file, const seismic_frame_t *frames, size_t count, size_t *written) { (void)file; (void)frames; *written = count; return ESP_OK; }
esp_err_t seismic_recorder_fs_flush(FILE *file, bool sync_to_media) { (void)file; (void)sync_to_media; return ESP_OK; }
esp_err_t seismic_recorder_fs_sync(FILE *file) { (void)file; return ESP_OK; }
esp_err_t seismic_recorder_fs_list_sessions(const char *object_id, seismic_recorder_session_summary_t *sessions, size_t max, size_t *count) { (void)object_id; (void)sessions; (void)max; *count = 0U; return ESP_OK; }
esp_err_t seismic_recorder_json_read_summary(const char *object_id, const char *session_id, seismic_recorder_session_summary_t *summary) { (void)object_id; (void)session_id; (void)summary; return ESP_ERR_NOT_FOUND; }
esp_err_t seismic_recorder_fs_build_session_path(const char *object_id, const char *session_id, char *path, size_t path_size) { (void)object_id; (void)session_id; (void)path; (void)path_size; return ESP_ERR_NOT_FOUND; }
esp_err_t seismic_recorder_waveform_read_file(FILE *file, uint64_t size, const seismic_recorder_session_summary_t *summary, uint64_t start, uint64_t count, size_t max_points, seismic_recorder_waveform_point_t *points, size_t capacity, seismic_recorder_waveform_info_t *info) { (void)file; (void)size; (void)summary; (void)start; (void)count; (void)max_points; (void)points; (void)capacity; (void)info; return ESP_ERR_NOT_FOUND; }

int main(void)
{
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
    assert(!last_written_profile_set);

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
    assert(!last_written_profile_set);

    /* Three assigned, leveled, altitude-reporting REMOTE pods on channels
     * 0-2, plus MAIN -- the MAX-165 minimum ("exactly 3 = allowed, planar/2D
     * only") rather than MAX-118's older, now-superseded 1-REMOTE minimum.
     * Reuses the exact mirror-disambiguation 3-4-5-triangle mesh from
     * nrf_geometry_solver_test.c's test_trilateration_with_mirror_disambiguation
     * (MAIN=(0,0), A=(300,0) via a single reference, B=(150,200) and
     * C=(150,-200) distinguished only via their own 400mm pair) so the
     * geometry math here is known-correct, not re-derived. */
    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.main.node_id = 0U;
    fake_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[0].node_id = 1U;
    fake_topology.remotes[0].mux_channel = 0U;
    fake_topology.remotes[1].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[1].node_id = 2U;
    fake_topology.remotes[1].mux_channel = 1U;
    fake_topology.remotes[2].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.remotes[2].node_id = 3U;
    fake_topology.remotes[2].mux_channel = 2U;

    fake_leveling.assigned_count = 3U;
    fake_leveling.accepted_count = 3U;

    memset(fake_sensor_cache, 0, sizeof(fake_sensor_cache));
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        fake_sensor_cache[channel].response_valid = true;
        fake_sensor_cache[channel].topology_current = true;
        fake_sensor_cache[channel].sensor.altitude_mm = 0;
        fake_sensor_cache[channel].sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
    }

    memset(&fake_cycle, 0, sizeof(fake_cycle));
    fake_cycle.active = false;
    fake_cycle.complete = true;
    fake_cycle.result = NRF_CS_AUTHORITY_COMPLETE;
    fake_cycle.pair_count = 6U;
    fake_cycle.pairs[0] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID, .distance_mm = 300 };
    fake_cycle.pairs[1] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[2] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[3] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[4] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[5] = (nrf_cs_pair_result_t){ .node_a = 2U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 400 };

    assert(seismic_recorder_init() == ESP_OK);
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);
    assert(last_geometry_set);
    assert(!last_geometry.anchored);
    assert(last_geometry.pods[0].computed && last_geometry.pods[0].leveled);
    assert(last_geometry.pods[0].x_mm == 300 && last_geometry.pods[0].y_mm == 0);
    assert(last_geometry.pods[1].computed && last_geometry.pods[1].leveled);
    assert(abs(last_geometry.pods[1].x_mm - 150) <= 2 && abs(last_geometry.pods[1].y_mm - 200) <= 2);
    assert(last_geometry.pods[2].computed && last_geometry.pods[2].leveled);
    /* The mirror-disambiguated point, not (150,200) again. */
    assert(abs(last_geometry.pods[2].x_mm - 150) <= 2 && abs(last_geometry.pods[2].y_mm - (-200)) <= 2);

    /* MAX-166: the profile handed to seismic_recorder_json_write_metadata()
     * (which freezes it into session.json) must reflect the geometry this
     * same recording just solved, not a stale pre-solve fetch. */
    assert(last_written_profile_set);
    assert(last_written_profile.geometry.pods[0].computed);
    assert(last_written_profile.geometry.pods[0].leveled);
    assert(last_written_profile.geometry.pods[0].x_mm == 300);

    /* MAX-148: the same "solve first, persist after" ordering must hold for
     * the CS pair evidence behind that solve. */
    assert(last_cs_pairs_set);
    assert(last_cs_pairs.pair_count == 6U);
    assert(last_cs_pairs.pairs[0].node_a == 0U && last_cs_pairs.pairs[0].node_b == 1U);
    assert(last_cs_pairs.pairs[0].valid);
    assert(last_cs_pairs.pairs[0].distance_mm == 300);
    assert(last_written_profile.cs_pairs.pair_count == 6U);
    assert(last_written_profile.cs_pairs.pairs[0].distance_mm == 300);

    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_RECORDING);
    assert(strcmp(snapshot.object_id, "OBJ_0005") == 0);
    assert(!snapshot.cadence.available);

    fake_acquisition.timer_ticks = 106U;
    fake_acquisition.conversion_attempts = 104U;
    fake_acquisition.collapsed_periods = 2U;

    assert(seismic_recorder_stop() == ESP_OK);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_STOPPING);
    assert(snapshot.cadence.available);
    assert(snapshot.cadence.timer_ticks == 6U);
    assert(snapshot.cadence.conversion_attempts == 4U);
    assert(snapshot.cadence.collapsed_periods == 2U);
    return 0;
}
