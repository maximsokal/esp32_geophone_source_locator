/*
 * ACTIVE_RECORDING allocation audit
 *
 * Boundary: after seismic_recorder_start() returns ESP_OK, with the recording
 * gate/session committed, acquisition running, recorder in RUNNING, and the
 * seismic ring plus recorder batch/cache/task preallocated. It ends when
 * seismic_recorder_stop() changes the state to STOPPING/DRAIN. Profile,
 * JSON/session setup during START and final metadata/close during STOP are
 * outside that window.
 *
 * The project-owned hot path is acquisition push -> ring copy -> cache append ->
 * dirty copy/write/persist -> heartbeat. Those functions use preallocated or
 * caller-owned buffers. Acquisition setup and recorder init allocate buffers
 * and create tasks; START performs profile/session/JSON work before RUNNING.
 * Localization DSP/event analysis is submitted through a separate
 * post-recording job; live UI/web reads are separate consumers. This
 * claim excludes ESP-IDF, libc/stdio, SD-driver, Wi-Fi/lwIP internals and other
 * independent request workers.
 *
 * The host allocator macros below interpose malloc/calloc/realloc/free in the
 * real recorder, cache, ring and JSON translation units. The existing START
 * integration fakes are reused, while their buffer/task/write stubs are
 * renamed so this target exercises the real seismic_buffer implementation.
 */

#undef malloc
#undef calloc
#undef realloc
#undef free
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_stop_timeout_config.h"

static size_t s_allocation_calls;
static size_t s_free_calls;
static size_t s_allocation_attempts;
static size_t s_fail_on_attempt;
static size_t s_post_init_allocations;
static size_t s_post_init_frees;
static size_t s_task_create_calls;
static bool s_failure_triggered;
static bool s_active_guard;
static size_t s_active_allocation_baseline;
static size_t s_active_free_baseline;
static size_t s_active_task_create_baseline;
static size_t s_active_violations;

static bool allocation_should_fail(void)
{
    ++s_allocation_attempts;
    if (s_fail_on_attempt != 0U &&
        s_allocation_attempts == s_fail_on_attempt) {
        s_failure_triggered = true;
        return true;
    }
    return false;
}

void *seismic_recorder_contract_malloc(size_t size)
{
    ++s_allocation_calls;
    if (s_active_guard) ++s_active_violations;
    return allocation_should_fail() ? NULL : malloc(size);
}

void *seismic_recorder_contract_calloc(size_t count, size_t size)
{
    ++s_allocation_calls;
    if (s_active_guard) ++s_active_violations;
    return allocation_should_fail() ? NULL : calloc(count, size);
}

void *seismic_recorder_contract_realloc(void *memory, size_t size)
{
    ++s_allocation_calls;
    if (s_active_guard) ++s_active_violations;
    return allocation_should_fail() ? NULL : realloc(memory, size);
}

void seismic_recorder_contract_free(void *memory)
{
    if (s_active_guard) ++s_active_violations;
    if (memory != NULL) ++s_free_calls;
    free(memory);
}

#define malloc seismic_recorder_contract_malloc
#define calloc seismic_recorder_contract_calloc
#define realloc seismic_recorder_contract_realloc
#define free seismic_recorder_contract_free

#undef seismic_recorder_start
#undef seismic_recorder_stop
#define main(...) seismic_recorder_start_integration_harness_main(__VA_ARGS__)
#define seismic_buffer_copy_latest seismic_recorder_harness_copy_latest
#define seismic_buffer_copy_since seismic_recorder_harness_copy_since
#define seismic_buffer_copy_since_with_source_indices seismic_recorder_harness_copy_since_with_indices
#define vTaskDelay seismic_recorder_harness_vTaskDelay
#define xTaskCreatePinnedToCore seismic_recorder_harness_create_task
#define seismic_recorder_fs_write_frames seismic_recorder_harness_write_frames
#include "../../../tests/host/seismic_recorder_start_integration_test.c"
#undef main
#undef seismic_buffer_copy_latest
#undef seismic_buffer_copy_since
#undef seismic_buffer_copy_since_with_source_indices
#undef vTaskDelay
#undef xTaskCreatePinnedToCore
#undef seismic_recorder_fs_write_frames

#include "seismic_recorder_cache.h"

esp_err_t seismic_recorder_start_unwrapped(const char *object_id);
esp_err_t seismic_recorder_stop_unwrapped(void);
BaseType_t seismic_recorder_harness_create_task(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
);

static bool s_fail_task_creation;
static bool s_yield_worker_after_delay;
static size_t s_persisted_frames;
static size_t s_last_write_count;
static uint64_t s_last_write_sample_indices[SEISMIC_RECORDER_BATCH_FRAMES];
static seismic_frame_t s_last_write_frames[SEISMIC_RECORDER_BATCH_FRAMES];
static void active_allocation_guard_begin(void);
static bool active_allocation_guard_holds(void);

esp_err_t seismic_recorder_start(const char *object_id)
{
    const esp_err_t err = seismic_recorder_start_unwrapped(object_id);
    if (err == ESP_OK) active_allocation_guard_begin();
    return err;
}

esp_err_t seismic_recorder_stop(void)
{
    const esp_err_t err = seismic_recorder_stop_unwrapped();
    if (err == ESP_OK) s_active_guard = false;
    return err;
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
    if (s_active_guard) ++s_active_violations;
    if (s_fail_task_creation) return 0;
    return seismic_recorder_harness_create_task(
        task, name, stack_size, argument, priority, task_handle, core_id
    );
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return NULL;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (final_metadata_written || s_yield_worker_after_delay) {
        longjmp(worker_finished, 1);
    }
}

esp_err_t seismic_recorder_fs_write_frames(
    FILE *file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t count,
    size_t *written
)
{
    (void)file;
    assert(frames != NULL && sample_indices != NULL && written != NULL);
    assert(count <= SEISMIC_RECORDER_BATCH_FRAMES);
    memcpy(s_last_write_frames, frames, count * sizeof(*frames));
    memcpy(
        s_last_write_sample_indices,
        sample_indices,
        count * sizeof(*sample_indices)
    );
    s_last_write_count = count;
    s_persisted_frames += count;
    *written = count;
    return ESP_OK;
}

static void reset_allocation_failure(size_t nth)
{
    s_allocation_attempts = 0U;
    s_fail_on_attempt = nth;
    s_failure_triggered = false;
}

static void assert_no_partial_recorder(void)
{
    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
}

static void test_recorder_init_failures(void)
{
    for (size_t nth = 5U; nth > 0U; --nth) {
        const size_t frees_before = s_free_calls;
        reset_allocation_failure(nth);
        assert(seismic_recorder_init() == ESP_ERR_NO_MEM);
        assert(s_failure_triggered);
        assert(s_allocation_attempts == nth);
        assert(s_free_calls - frees_before == nth - 1U);
        assert_no_partial_recorder();
    }

    const size_t task_failure_frees_before = s_free_calls;
    reset_allocation_failure(0U);
    s_fail_task_creation = true;
    assert(seismic_recorder_init() == ESP_ERR_NO_MEM);
    s_fail_task_creation = false;
    assert(s_allocation_attempts == 5U);
    assert(s_free_calls - task_failure_frees_before == 5U);
    assert_no_partial_recorder();

    reset_allocation_failure(0U);
    assert(seismic_recorder_init() == ESP_OK);
    assert(s_allocation_attempts == 5U);
    assert(fake_recorder_task != NULL);
    s_post_init_allocations = s_allocation_calls;
    s_post_init_frees = s_free_calls;
}

static void prepare_successful_recording_fixture(void)
{
    memset(&fake_acquisition, 0, sizeof(fake_acquisition));
    fake_acquisition.running = true;
    fake_acquisition.source_healthy = true;
    fake_acquisition.source_name = "AD7606";
    fake_acquisition.sample_rate_hz = 500U;
    fake_acquisition.latest_valid = true;
    fake_acquisition.samples = 1U;
    fake_acquisition.last_sample_timestamp_us = 999000;

    memset(&fake_topology, 0, sizeof(fake_topology));
    fake_topology.generation = 143U;
    fake_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    fake_topology.main.node_id = 0U;
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
    fake_device_status.main.height_reference.state =
        DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
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
    fake_cycle.pairs[0] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 300,
    };
    fake_cycle.pairs[1] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 2U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 250,
    };
    fake_cycle.pairs[2] = (nrf_cs_pair_result_t){
        .node_a = 1U, .node_b = 2U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 250,
    };
    fake_cycle.pairs[3] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 3U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 250,
    };
    fake_cycle.pairs[4] = (nrf_cs_pair_result_t){
        .node_a = 1U, .node_b = 3U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 250,
    };
    fake_cycle.pairs[5] = (nrf_cs_pair_result_t){
        .node_a = 2U, .node_b = 3U, .state = NRF_CS_PAIR_VALID,
        .distance_mm = 400,
    };

    /* START takes the geometry the operator confirmed; it no longer measures one. */
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
}

static void assert_start_failure_is_clean(void)
{
    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.initialized);
    assert(!snapshot.running);
    assert(snapshot.state != SEISMIC_RECORDER_RECORDING);
    assert(snapshot.state != SEISMIC_RECORDER_STOPPING);
}

static size_t measure_start_allocation_attempts(void)
{
    reset_allocation_failure(0U);
    assert(s_allocation_calls == s_post_init_allocations);
    assert(s_free_calls == s_post_init_frees);
    final_metadata_written = false;
    const size_t allocations_before = s_allocation_calls;
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);
    assert(s_active_guard);
    const size_t attempts = s_allocation_attempts;
    assert(attempts > 0U);
    assert(s_allocation_calls - allocations_before == attempts);
    assert(seismic_recorder_stop() == ESP_OK);
    s_yield_worker_after_delay = false;
    if (setjmp(worker_finished) == 0) fake_recorder_task(NULL);
    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    assert_start_failure_is_clean();
    return attempts;
}

static void test_start_allocation_failures(size_t allocation_points)
{
    for (size_t nth = 1U; nth <= allocation_points; ++nth) {
        reset_allocation_failure(nth);
        final_metadata_written = false;
        const size_t allocations_before = s_allocation_calls;
        const esp_err_t err = seismic_recorder_start("OBJ_0005");
        assert(s_failure_triggered);
        assert(s_allocation_attempts >= nth);
        assert(s_allocation_calls - allocations_before == s_allocation_attempts);
        if (err == ESP_ERR_NO_MEM) {
            assert(s_allocation_attempts == nth);
        } else {
            assert(err == ESP_OK);
            assert(active_allocation_guard_holds());
            assert(seismic_recorder_stop() == ESP_OK);
            s_yield_worker_after_delay = false;
            final_metadata_written = false;
            if (setjmp(worker_finished) == 0) fake_recorder_task(NULL);
            assert(seismic_recorder_wait_for_stop() == ESP_OK);
        }
        assert_start_failure_is_clean();
    }
    reset_allocation_failure(0U);
}

static void active_allocation_guard_begin(void)
{
    s_active_violations = 0U;
    s_active_allocation_baseline = s_allocation_calls;
    s_active_free_baseline = s_free_calls;
    s_active_task_create_baseline = s_task_create_calls;
    s_active_guard = true;
}

static bool active_allocation_guard_holds(void)
{
    return s_active_guard &&
        s_active_violations == 0U &&
        s_allocation_calls == s_active_allocation_baseline &&
        s_free_calls == s_active_free_baseline &&
        s_task_create_calls == s_active_task_create_baseline;
}

static void assert_active_allocation_guard(void)
{
    assert(active_allocation_guard_holds());
}

#if defined(SEISMIC_RECORDER_ALLOCATION_NEGATIVE_CONTROL)
static void forbidden_active_allocation(void)
{
    const size_t allocations_before = s_allocation_calls;
    assert(s_active_guard);
    void *pointer = malloc(16U);
    assert(pointer != NULL);
    assert(s_allocation_calls == allocations_before + 1U);
    assert(s_active_violations == 1U);
    assert(!active_allocation_guard_holds());
    s_active_guard = false;
    free(pointer);
}
#endif

static void push_recording_frames(uint64_t first, size_t count)
{
    float channels[SEISMIC_CHANNEL_COUNT] = {0};
    for (size_t i = 0U; i < count; ++i) {
        const uint64_t sample_index = first + (uint64_t)i;
        channels[0] = (float)sample_index;
        channels[1] = (float)sample_index + 0.25f;
        channels[2] = (float)sample_index + 0.5f;
        channels[3] = (float)sample_index + 0.75f;
        assert(seismic_buffer_push_indexed(
            998000LL + (int64_t)sample_index * 2000LL,
            sample_index,
            channels
        ) == ESP_OK);
    }
}

static void run_active_worker_turn(void)
{
    assert(fake_recorder_task != NULL);
    s_yield_worker_after_delay = true;
    if (setjmp(worker_finished) == 0) fake_recorder_task(NULL);
    s_yield_worker_after_delay = false;
}

static void test_active_recording_contract(void)
{
    const size_t allocation_points = measure_start_allocation_attempts();
    test_start_allocation_failures(allocation_points);

    reset_allocation_failure(0U);
    final_metadata_written = false;
    s_persisted_frames = 0U;
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);
    seismic_recorder_snapshot_t active_snapshot;
    assert(seismic_recorder_get_snapshot(&active_snapshot) == ESP_OK);
    assert(active_snapshot.running);
    assert(active_snapshot.state == SEISMIC_RECORDER_RECORDING);
    assert(seismic_buffer_capacity() > 0U);
    active_allocation_guard_begin();

    for (size_t batch = 0U; batch < 3U; ++batch) {
        push_recording_frames(1U + (uint64_t)batch * 250U, 250U);
        assert_active_allocation_guard();
        run_active_worker_turn();
        assert_active_allocation_guard();
        seismic_recorder_snapshot_t snapshot;
        assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.running);
        assert(snapshot.samples_written == (batch + 1U) * 250U);
        assert(s_last_write_count == 250U);
    }

    push_recording_frames(751U, 7U);
    assert_active_allocation_guard();
#if defined(SEISMIC_RECORDER_ALLOCATION_NEGATIVE_CONTROL)
    forbidden_active_allocation();
    assert(!s_active_guard);
    assert(s_active_violations == 1U);
#endif

    assert(seismic_recorder_stop() == ESP_OK);
    s_yield_worker_after_delay = false;
    final_metadata_written = false;
    if (setjmp(worker_finished) == 0) fake_recorder_task(NULL);
    assert(seismic_recorder_wait_for_stop() == ESP_OK);

    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_IDLE);
    assert(snapshot.samples_written == 757U);
    assert(snapshot.dropped_frames == 0U);
    assert(s_persisted_frames == 757U);
    assert(s_last_write_count == 7U);
    assert(s_last_write_sample_indices[0] == 751U);
    assert(s_last_write_sample_indices[6] == 757U);
    assert(s_last_write_frames[6].channel_uv[0] == 757.0f);
}

static void test_stop_timeout_marks_recording_incomplete(void)
{
    reset_allocation_failure(0U);
    final_metadata_written = false;
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);
    assert(seismic_recorder_stop() == ESP_OK);

    s_force_event_wait_timeout = true;
    assert(seismic_recorder_wait_for_stop() == ESP_ERR_TIMEOUT);
    assert(s_last_event_wait_timeout == pdMS_TO_TICKS(APP_STOP_TIMEOUT_RECORDING_MS));
    s_force_event_wait_timeout = false;

    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_STOPPING);
    assert(snapshot.last_error == ESP_ERR_TIMEOUT);
    assert(snapshot.stop_stage_error == ESP_ERR_TIMEOUT);

    if (setjmp(worker_finished) == 0) fake_recorder_task(NULL);
    assert(seismic_recorder_wait_for_stop() == ESP_ERR_TIMEOUT);
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_ERROR);
    assert(snapshot.last_error == ESP_ERR_TIMEOUT);
}

static void assert_cache_conservation(
    const seismic_recorder_cache_t *cache,
    uint64_t produced,
    uint64_t persisted,
    uint64_t rejected
)
{
    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(cache, &info) == ESP_OK);
    assert(produced == persisted + info.dirty_frames + rejected);
}

static void test_cache_exhaustion_accounting(void)
{
    const size_t allocations_before = s_allocation_calls;
    const size_t frees_before = s_free_calls;
    seismic_recorder_cache_slot_t storage[2];
    seismic_recorder_cache_t cache;
    seismic_frame_t frames[4] = {0};
    uint64_t sample_indices[2] = {0};
    size_t copied = 0U;
    uint64_t first_session_frame = 0U;
    uint64_t last_source_frame = 0U;
    uint64_t produced = 0U;
    uint64_t persisted = 0U;
    uint64_t rejected = 0U;

    frames[0].timestamp_us = 10;
    frames[1].timestamp_us = 20;
    frames[2].timestamp_us = 30;
    frames[3].timestamp_us = 40;
    assert(seismic_recorder_cache_init(&cache, storage, 2U) == ESP_OK);
    assert(seismic_recorder_cache_append(&cache, 1U, frames, 2U) == ESP_OK);
    produced += 2U;
    assert_cache_conservation(&cache, produced, persisted, rejected);

    assert(seismic_recorder_cache_append(&cache, 3U, &frames[2], 1U) == ESP_ERR_NO_MEM);
    ++produced;
    ++rejected;
    assert_cache_conservation(&cache, produced, persisted, rejected);
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache, frames, sample_indices, 2U, &copied, &first_session_frame
    ) == ESP_OK);
    assert(copied == 2U && frames[0].timestamp_us == 10 && frames[1].timestamp_us == 20);
    assert(sample_indices[0] == 1U && sample_indices[1] == 2U);
    assert(seismic_recorder_cache_mark_persisted(
        &cache, first_session_frame, 1U, &last_source_frame
    ) == ESP_OK);
    ++persisted;
    assert_cache_conservation(&cache, produced, persisted, rejected);

    assert(seismic_recorder_cache_append(&cache, 3U, &frames[2], 1U) == ESP_OK);
    --rejected;
    assert_cache_conservation(&cache, produced, persisted, rejected);

    assert(seismic_recorder_cache_append(&cache, 4U, &frames[3], 1U) == ESP_ERR_NO_MEM);
    ++produced;
    ++rejected;
    assert_cache_conservation(&cache, produced, persisted, rejected);
    assert(seismic_recorder_cache_copy_dirty(
        &cache, frames, 2U, &copied, &first_session_frame
    ) == ESP_OK);
    assert(copied == 2U);
    assert(frames[0].timestamp_us == 20 && frames[1].timestamp_us == 30);
    assert(seismic_recorder_cache_mark_persisted(
        &cache, first_session_frame, copied, &last_source_frame
    ) == ESP_OK);
    persisted += copied;
    assert_cache_conservation(&cache, produced, persisted, rejected);

    assert(seismic_recorder_cache_append(&cache, 4U, &frames[3], 1U) == ESP_OK);
    --rejected;
    assert_cache_conservation(&cache, produced, persisted, rejected);
    assert(s_allocation_calls == allocations_before);
    assert(s_free_calls == frees_before);
}

int main(void)
{
    prepare_successful_recording_fixture();
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    float initial_channels[SEISMIC_CHANNEL_COUNT] = {0};
    assert(seismic_buffer_push_indexed(998000, 0U, initial_channels) == ESP_OK);

    test_recorder_init_failures();
    test_active_recording_contract();
#if !defined(SEISMIC_RECORDER_ALLOCATION_NEGATIVE_CONTROL)
    test_stop_timeout_marks_recording_incomplete();
#endif
    test_cache_exhaustion_accounting();
#if defined(SEISMIC_RECORDER_ALLOCATION_NEGATIVE_CONTROL)
    assert(s_active_violations == 1U);
    puts("negative control passed: guard detected one active-window allocation");
    return 0;
#else
    assert(s_active_violations == 0U);
    puts("seismic recorder allocation contract passed");
    return 0;
#endif
}
