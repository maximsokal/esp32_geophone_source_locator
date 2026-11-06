#undef malloc
#undef calloc
#undef realloc
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_status.h"
#include "nrf_cs_authority.h"
#include "nrf_topology_service.h"
#include "seismic_acquisition.h"
#include "seismic_recorder.h"
#include "storage_manager.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

typedef enum {
    FAULT_NONE = 0,
    FAULT_SESSION_CREATE,
    FAULT_START_METADATA,
    FAULT_STOP_FLUSH,
    FAULT_STOP_SYNC,
    FAULT_STOP_CLOSE,
    FAULT_FINAL_METADATA,
} fault_point_t;

static bool s_fail_next_malloc;
static size_t s_allocation_attempts;
static size_t s_fail_allocation_nth;
static size_t s_failed_allocations;
static bool s_shutdown_worker_capture;
static size_t s_task_create_calls;
static size_t s_session_create_calls;
static size_t s_session_close_calls;
static FILE *s_open_session_file;
static bool s_fail_acquisition_snapshot;
static bool s_fail_topology_snapshot;
static bool s_fail_sensor_snapshot;
static unsigned s_topology_snapshot_calls;
static unsigned s_sensor_snapshot_calls;
static unsigned s_injected_storage_errors;
static fault_point_t s_fault;

static bool seismic_recorder_fault_should_fail(void)
{
    ++s_allocation_attempts;
    if (s_fail_next_malloc ||
        (s_fail_allocation_nth != 0U &&
         s_allocation_attempts == s_fail_allocation_nth)) {
        s_fail_next_malloc = false;
        ++s_failed_allocations;
        return true;
    }
    return false;
}

void *seismic_recorder_fault_malloc(size_t size)
{
    return seismic_recorder_fault_should_fail() ? NULL : malloc(size);
}

void *seismic_recorder_fault_calloc(size_t count, size_t size)
{
    return seismic_recorder_fault_should_fail() ? NULL : calloc(count, size);
}

void *seismic_recorder_fault_realloc(void *memory, size_t size)
{
    return seismic_recorder_fault_should_fail() ? NULL : realloc(memory, size);
}

#define malloc seismic_recorder_fault_malloc
#define calloc seismic_recorder_fault_calloc
#define realloc seismic_recorder_fault_realloc
#define main(...) max444_recording_gate_fixture_main(__VA_ARGS__)
#define seismic_acquisition_get_snapshot max444_acquisition_get_snapshot
#define nrf_topology_service_get_snapshot max444_topology_get_snapshot
#define nrf_topology_service_get_sensor_snapshot max444_sensor_get_snapshot
#define seismic_recorder_fs_create_session max444_fs_create_session
#define seismic_recorder_fs_close max444_fs_close
#define seismic_recorder_fs_flush max444_fs_flush
#define seismic_recorder_fs_sync max444_fs_sync
#define storage_manager_atomic_write_text max444_storage_atomic_write_text
#define vTaskDelay max482b_original_vTaskDelay
#define xTaskCreatePinnedToCore max482b_original_xTaskCreatePinnedToCore
#include "seismic_recorder_start_integration_test.c"
#undef storage_manager_atomic_write_text
#undef seismic_recorder_fs_sync
#undef seismic_recorder_fs_flush
#undef seismic_recorder_fs_close
#undef seismic_recorder_fs_create_session
#undef nrf_topology_service_get_sensor_snapshot
#undef nrf_topology_service_get_snapshot
#undef seismic_acquisition_get_snapshot
#undef main
#undef vTaskDelay
#undef xTaskCreatePinnedToCore
#undef realloc
#undef calloc
#undef malloc

void vTaskDelay(TickType_t ticks)
{
    max482b_original_vTaskDelay(ticks);
    if (!s_shutdown_worker_capture) return;

    seismic_recorder_snapshot_t snapshot;
    if (seismic_recorder_get_snapshot(&snapshot) == ESP_OK &&
        !snapshot.running && snapshot.state == SEISMIC_RECORDER_ERROR &&
        snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_ERROR) {
        longjmp(worker_finished, 1);
    }
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
)
{
    ++s_task_create_calls;
    return max482b_original_xTaskCreatePinnedToCore(
        task, name, stack_size, argument, priority, task_handle, core_id
    );
}

esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot)
{
    if (s_fail_acquisition_snapshot) return ESP_ERR_NOT_FOUND;
    return max444_acquisition_get_snapshot(snapshot);
}

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{
    ++s_topology_snapshot_calls;
    if (s_fail_topology_snapshot) return ESP_ERR_NOT_FOUND;
    return max444_topology_get_snapshot(snapshot);
}

esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot)
{
    ++s_sensor_snapshot_calls;
    if (s_fail_sensor_snapshot) return ESP_ERR_NOT_FOUND;
    return max444_sensor_get_snapshot(channel, snapshot);
}

esp_err_t seismic_recorder_fs_create_session(
    const char *object_id,
    const seismic_acquisition_snapshot_t *acquisition,
    uint8_t channels,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **file,
    uint16_t *format_version)
{
    if (s_fault == FAULT_SESSION_CREATE) {
        ++s_injected_storage_errors;
        return ESP_FAIL;
    }
    const esp_err_t err = max444_fs_create_session(
        object_id, acquisition, channels, session_id, file, format_version
    );
    if (err == ESP_OK) {
        assert(file != NULL && *file != NULL && s_open_session_file == NULL);
        s_open_session_file = *file;
        ++s_session_create_calls;
    }
    return err;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (s_fault == FAULT_START_METADATA && text != NULL && strstr(text, "\"status\":\"recording\"") != NULL) {
        ++s_injected_storage_errors;
        return ESP_FAIL;
    }
    if (s_fault == FAULT_FINAL_METADATA && text != NULL && strstr(text, "\"status\":\"complete\"") != NULL) {
        ++s_injected_storage_errors;
        final_metadata_written = true;
        return ESP_FAIL;
    }
    return max444_storage_atomic_write_text(path, text);
}

esp_err_t seismic_recorder_fs_flush(FILE *file, bool sync_to_media)
{
    if (s_fault == FAULT_STOP_FLUSH && !sync_to_media) return ESP_FAIL;
    return max444_fs_flush(file, sync_to_media);
}

esp_err_t seismic_recorder_fs_sync(FILE *file)
{
    if (s_fault == FAULT_STOP_SYNC) return ESP_FAIL;
    return max444_fs_sync(file);
}

esp_err_t seismic_recorder_fs_close(FILE *file)
{
    assert(file != NULL && file == s_open_session_file);
    const esp_err_t close_result = max444_fs_close(file);
    s_open_session_file = NULL;
    ++s_session_close_calls;
    return s_fault == FAULT_STOP_CLOSE ? ESP_FAIL : close_result;
}

static void configure_valid_start(void)
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

    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.generation = 143U;
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        fake_topology.remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        fake_topology.remotes[channel].node_id = (uint8_t)(channel + 1U);
        fake_topology.remotes[channel].mux_channel = channel;
        fake_topology.remotes[channel].hardware_id_length = 2U;
        fake_topology.remotes[channel].hardware_id[0] = (uint8_t)(0xA0U + channel);
        fake_topology.remotes[channel].hardware_id[1] = (uint8_t)(0x10U + channel);
    }
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
    fake_cycle.complete = true;
    fake_cycle.result = NRF_CS_AUTHORITY_COMPLETE;
    fake_cycle.membership_generation = 52U;
    fake_cycle.pair_count = 6U;
    fake_cycle.pairs[0] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID, .distance_mm = 300 };
    fake_cycle.pairs[1] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[2] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 2U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[3] = (nrf_cs_pair_result_t){ .node_a = 0U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[4] = (nrf_cs_pair_result_t){ .node_a = 1U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 250 };
    fake_cycle.pairs[5] = (nrf_cs_pair_result_t){ .node_a = 2U, .node_b = 3U, .state = NRF_CS_PAIR_VALID, .distance_mm = 400 };
#if !CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
    fake_leveling.assigned_count = 2U;
    fake_leveling.accepted_count = 2U;
    fake_reusable_ranging_available = false;
#else
    memset(&fake_frozen_ranging, 0, sizeof(fake_frozen_ranging));
    fake_frozen_ranging.available = fake_frozen_ranging.success = true;
    fake_frozen_ranging.timestamp_ms = 1000U;
    fake_frozen_ranging.topology_generation = 143U;
    fake_frozen_ranging.membership_generation = 52U;
    fake_frozen_ranging.main_height_reference_generation = 7U;
    fake_frozen_ranging.pair_count = fake_cycle.pair_count;
    memcpy(fake_frozen_ranging.pairs, fake_cycle.pairs, sizeof(fake_cycle.pairs));
    fake_placement_generation = 7U;
    memset(fake_placement_valid, 0, sizeof(fake_placement_valid));
    memset(fake_placement_height_mm, 0, sizeof(fake_placement_height_mm));
    for (size_t i = 1U; i <= 3U; ++i) fake_placement_valid[i] = true;
    fake_confirm_current_geometry();
#endif
    fake_health_deadline_armed = false;
    fake_ring_frame_count = 0U;
    fake_ring_total_frames = 0U;
    fake_ring_dropped_frames = 0U;
    fake_ring_batch_pending = false;
    final_metadata_written = false;
    metadata_write_calls = 0U;
    s_injected_storage_errors = 0U;
    persisted_session_json[0] = '\0';
    last_metadata_status[0] = '\0';
    s_fail_acquisition_snapshot = false;
    s_fail_topology_snapshot = false;
    s_fail_sensor_snapshot = false;
    s_topology_snapshot_calls = 0U;
    s_sensor_snapshot_calls = 0U;
    s_fault = FAULT_NONE;
    s_allocation_attempts = 0U;
    s_fail_allocation_nth = 0U;
    s_failed_allocations = 0U;
    s_fail_next_malloc = false;
}

static seismic_recorder_snapshot_t current_snapshot(void)
{
    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

static void assert_failure_not_accepted(void)
{
    const seismic_recorder_snapshot_t snapshot = current_snapshot();
    assert(!snapshot.running);
    assert(snapshot.state != SEISMIC_RECORDER_RECORDING);
    assert(strstr(persisted_session_json, "\"status\":\"complete\"") == NULL);
}

static void run_worker_to_final_metadata(void)
{
    final_metadata_written = false;
    assert(fake_recorder_task != NULL);
    s_shutdown_worker_capture = true;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "recorder worker did not reach final metadata");
    }
    s_shutdown_worker_capture = false;
    assert(final_metadata_written);
}

static void run_worker_to_shutdown_result(void)
{
    final_metadata_written = false;
    assert(fake_recorder_task != NULL);
    s_shutdown_worker_capture = true;
    if (setjmp(worker_finished) == 0) {
        fake_recorder_task(NULL);
        assert(false && "recorder worker did not reach a shutdown result");
    }
    s_shutdown_worker_capture = false;
}

static void vary_host_allocation_order(size_t seed)
{
    void *blocks[4] = {0};
    const size_t count = 1U + seed % 4U;
    for (size_t index = 0U; index < count; ++index) {
        blocks[index] = malloc(37U + seed * 3U + index * 19U);
        assert(blocks[index] != NULL);
    }
    for (size_t index = count; index > 0U; --index) {
        free(blocks[index - 1U]);
    }
}

static void test_repeated_start_stop_cycles(void)
{
    const size_t task_creates = s_task_create_calls;
    assert(s_open_session_file == NULL);
    assert(s_session_create_calls == s_session_close_calls);

    for (size_t cycle = 0U; cycle < 8U; ++cycle) {
        configure_valid_start();
        vary_host_allocation_order(cycle + 1U);

        if (cycle % 3U == 1U) {
            s_fail_next_malloc = true;
            assert(seismic_recorder_start("OBJ_0491") == ESP_ERR_NO_MEM);
            assert_failure_not_accepted();
            assert(s_open_session_file == NULL);
            assert(s_session_create_calls == s_session_close_calls);
            configure_valid_start();
        } else if (cycle % 3U == 2U) {
            s_fault = FAULT_START_METADATA;
            assert(seismic_recorder_start("OBJ_0491") == ESP_FAIL);
            assert_failure_not_accepted();
            assert(s_open_session_file == NULL);
            assert(s_session_create_calls == s_session_close_calls);
            configure_valid_start();
        }

        const size_t created_before = s_session_create_calls;
        assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
        assert(s_session_create_calls == created_before + 1U);
        assert(s_open_session_file != NULL);
        assert(s_task_create_calls == task_creates);
        assert(seismic_recorder_stop() == ESP_OK);
        run_worker_to_final_metadata();

        const seismic_recorder_snapshot_t snapshot = current_snapshot();
        assert(!snapshot.running);
        assert(snapshot.state == SEISMIC_RECORDER_IDLE);
        assert(snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_COMPLETE);
        assert(strcmp(last_metadata_status, "complete") == 0);
        assert(s_open_session_file == NULL);
        assert(s_session_create_calls == s_session_close_calls);
        assert(s_task_create_calls == task_creates);
    }
}

static void stop_successfully(void)
{
    s_fault = FAULT_NONE;
    assert(seismic_recorder_stop() == ESP_OK);
    run_worker_to_final_metadata();
    const seismic_recorder_snapshot_t snapshot = current_snapshot();
    assert(!snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_IDLE);
    assert(snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_COMPLETE);
    assert(strcmp(last_metadata_status, "complete") == 0);
}

/* MAX-491 mutation-only: early source, gate, topology, sensor and allocation faults are not accepted. */
static void test_early_failures_preserve_public_state(void)
{
    configure_valid_start();
    const seismic_recorder_snapshot_t before = current_snapshot();
    s_fail_acquisition_snapshot = true;
    assert(seismic_recorder_start("OBJ_0491") == ESP_ERR_INVALID_STATE);
    s_fail_acquisition_snapshot = false;
    seismic_recorder_snapshot_t after = current_snapshot();
    assert(memcmp(&after, &before, sizeof(before)) == 0);
    assert_failure_not_accepted();

    s_fail_next_malloc = true;
    assert(seismic_recorder_start("OBJ_0491") == ESP_ERR_NO_MEM);
    assert(!s_fail_next_malloc);
    after = current_snapshot();
    assert(memcmp(&after, &before, sizeof(before)) == 0);
    assert_failure_not_accepted();

#if CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
    s_fail_topology_snapshot = true;
    assert(seismic_recorder_start("OBJ_0491") == ESP_ERR_NOT_FOUND);
    s_fail_topology_snapshot = false;
    assert(s_topology_snapshot_calls == 1U);
    after = current_snapshot();
    assert(memcmp(&after, &before, sizeof(before)) == 0);
    assert_failure_not_accepted();

    s_fail_sensor_snapshot = true;
    assert(seismic_recorder_start("OBJ_0491") != ESP_OK);
    s_fail_sensor_snapshot = false;
    assert(s_sensor_snapshot_calls == 3U);
    after = current_snapshot();
    assert(memcmp(&after, &before, sizeof(before)) == 0);
    assert_failure_not_accepted();

    fake_leveling.assigned_count = 2U;
    fake_leveling.accepted_count = 2U;
    assert(seismic_recorder_start("OBJ_0491") == ESP_ERR_NOT_FOUND);
    after = current_snapshot();
    assert(memcmp(&after, &before, sizeof(before)) == 0);
#else
    s_fail_topology_snapshot = true;
    assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
    assert(s_topology_snapshot_calls == 0U && s_sensor_snapshot_calls == 0U);
    stop_successfully();
    configure_valid_start();
    s_fail_sensor_snapshot = true;
    assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
    assert(s_topology_snapshot_calls == 0U && s_sensor_snapshot_calls == 0U);
    stop_successfully();
#endif
}

/* MAX-491 mutation-only: create and initial metadata failures never publish a recording. */
static void test_session_creation_and_start_metadata_failures(void)
{
    configure_valid_start();
    s_fault = FAULT_SESSION_CREATE;
    assert(seismic_recorder_start("OBJ_0491") == ESP_FAIL);
    assert(s_injected_storage_errors == 1U);
    assert_failure_not_accepted();
    seismic_recorder_snapshot_t snapshot = current_snapshot();
    assert(snapshot.state == SEISMIC_RECORDER_ERROR);
    assert(snapshot.last_error == ESP_FAIL);

    configure_valid_start();
    s_fault = FAULT_START_METADATA;
    assert(seismic_recorder_start("OBJ_0491") == ESP_FAIL);
    assert(s_injected_storage_errors == 1U);
    assert_failure_not_accepted();
    snapshot = current_snapshot();
    assert(snapshot.state == SEISMIC_RECORDER_ERROR);
    assert(snapshot.last_error == ESP_FAIL);
}

/* MAX-491 mutation-only: a failed stop stage or final metadata write leaves an ERROR session. */
static void test_stop_stage_faults_never_mark_partial_data_complete(void)
{
    static const struct {
        fault_point_t fault;
        seismic_recorder_stop_stage_t stage;
    } cases[] = {
        { FAULT_STOP_FLUSH, SEISMIC_RECORDER_STOP_STAGE_FLUSH },
        { FAULT_STOP_SYNC, SEISMIC_RECORDER_STOP_STAGE_SYNC },
        { FAULT_STOP_CLOSE, SEISMIC_RECORDER_STOP_STAGE_CLOSE },
        { FAULT_FINAL_METADATA, SEISMIC_RECORDER_STOP_STAGE_METADATA },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        configure_valid_start();
        assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
        const seismic_recorder_snapshot_t accepted = current_snapshot();
        assert(accepted.running && accepted.state == SEISMIC_RECORDER_RECORDING);
        assert(strcmp(last_metadata_status, "recording") == 0);
        assert(seismic_recorder_stop() == ESP_OK);
        s_fault = cases[i].fault;
        run_worker_to_final_metadata();

        const seismic_recorder_snapshot_t finished = current_snapshot();
        assert(!finished.running);
        assert(finished.state == SEISMIC_RECORDER_ERROR);
        assert(finished.stop_stage == SEISMIC_RECORDER_STOP_STAGE_ERROR);
        assert(finished.stop_error_stage == cases[i].stage);
        assert(finished.stop_stage_error != ESP_OK);
        assert(strstr(persisted_session_json, "\"status\":\"complete\"") == NULL);
        if (cases[i].fault == FAULT_FINAL_METADATA) {
            assert(strcmp(last_metadata_status, "recording") == 0);
        } else {
            assert(strcmp(last_metadata_status, "error") == 0);
            assert(strstr(persisted_session_json, "\"status\":\"error\"") != NULL);
        }
    }
}

static size_t measure_successful_shutdown_allocations(void)
{
    configure_valid_start();
    assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
    assert(seismic_recorder_stop() == ESP_OK);
    s_allocation_attempts = 0U;
    s_fail_allocation_nth = 0U;
    s_failed_allocations = 0U;
    run_worker_to_final_metadata();
    assert(strcmp(last_metadata_status, "complete") == 0);
    const size_t allocations = s_allocation_attempts;
    assert(allocations > 0U);
    return allocations;
}

static void test_shutdown_allocation_failures_never_complete(void)
{
    const size_t allocation_points = measure_successful_shutdown_allocations();
    for (size_t nth = 1U; nth <= allocation_points; ++nth) {
        configure_valid_start();
        assert(seismic_recorder_start("OBJ_0491") == ESP_OK);
        assert(seismic_recorder_stop() == ESP_OK);
        s_allocation_attempts = 0U;
        s_fail_allocation_nth = nth;
        s_failed_allocations = 0U;
        run_worker_to_shutdown_result();

        assert(s_failed_allocations == 1U);
        assert_failure_not_accepted();
        const seismic_recorder_snapshot_t snapshot = current_snapshot();
        assert(snapshot.state == SEISMIC_RECORDER_ERROR);
        assert(snapshot.stop_stage == SEISMIC_RECORDER_STOP_STAGE_ERROR);
        assert(snapshot.stop_error_stage == SEISMIC_RECORDER_STOP_STAGE_METADATA);
        assert(snapshot.stop_stage_error == ESP_ERR_NO_MEM);
    }
}

int main(void)
{
    configure_valid_start();
    assert(seismic_recorder_init() == ESP_OK);
    test_early_failures_preserve_public_state();
    test_session_creation_and_start_metadata_failures();
    test_stop_stage_faults_never_mark_partial_data_complete();
    test_shutdown_allocation_failures_never_complete();
    test_repeated_start_stop_cycles();
    puts("seismic recorder fault FSM tests passed");
    return 0;
}
