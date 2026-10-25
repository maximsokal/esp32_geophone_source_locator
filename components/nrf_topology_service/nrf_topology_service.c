#include "nrf_topology_service.h"
#include "nrf_sensor_transport.h"
#include "nrf_cs_authority.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_log_protocol.h"
#include "nrf_link_readiness_policy.h"
#include "nrf_topology_periods.h"

#if defined(ESP_PLATFORM) || defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
#define NRF_TOPOLOGY_HAS_RANGING_SOLVER 1
#include "device_status.h"
#include "nrf_geometry_solver.h"
#else
#define NRF_TOPOLOGY_HAS_RANGING_SOLVER 0
#endif

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "sensor_bus.h"
#include "sensor_bus_diag.h"
#include "sensor_bus_probe_diag.h"
#include "nrf_topology_bus_recovery.h"
#include "nrf_topology_log_policy.h"
#include "tca9548a_mux.h"
#include "tca9548a_mux_i2c_bus.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"

#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
#include "esp_heap_caps.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NRF_TOPOLOGY_TIMEOUT_ESCALATION_WINDOWS 3U
#define NRF_TOPOLOGY_STACK_MARGIN_BYTES 1024U

#ifndef CONFIG_NRF_LOG_POLL_NODE_BUDGET_MS
#define CONFIG_NRF_LOG_POLL_NODE_BUDGET_MS 50U
#endif
#ifndef CONFIG_NRF_LOG_POLL_ITERATION_BUDGET_MS
#define CONFIG_NRF_LOG_POLL_ITERATION_BUDGET_MS 100U
#endif
static const char *TAG = "NRF_TOPOLOGY";
static const char s_i2c_diag_source[] = "nrf_topology";

#define NRF_PLATFORM_READ_ROUTE_COUNT (NRF_TOPOLOGY_REMOTE_COUNT + 1U)
#define NRF_PLATFORM_READ_ADDRESS_COUNT 3U

typedef struct {
    sensor_bus_handles_t bus_handles;
    i2c_bus_device_handle_t discovery_device;
    i2c_bus_device_handle_t main_device;
    i2c_bus_device_handle_t remote_device;
    i2c_bus_device_handle_t tca_device;
    uint8_t tca_address;
    tca9548a_mux_t mux;
    bool mux_bound;
    uint8_t selected_mux_channel;
    bool selected_mux_channel_valid;
    uint8_t requested_mux_channel;
    bool requested_mux_channel_valid;
    bool bus_owner_held;
    bool reconcile_pass_active;
    bool reconcile_idle_logged;
    uint32_t reconcile_pass_count;
    uint32_t timeout_error_pass;
    uint32_t timeout_error_windows;
    uint32_t timeout_recovery_pass;
    nrf_topology_tca_log_state_t tca_log_state;
    bool last_good_snapshot_valid;
    uint64_t last_good_snapshot_ms;
    nrf_topology_snapshot_t last_good_snapshot;
    nrf_topology_snapshot_t *active_snapshot;
    bool direct_conflict_snapshot_valid;
    nrf_topology_snapshot_t direct_conflict_snapshot;
    bool expected_main_identity_valid;
    nrf_topology_node_snapshot_t expected_main_identity;
    nrf_upstream_status_cache_t main_status_cache;
    bool read_probe_valid[NRF_PLATFORM_READ_ROUTE_COUNT][NRF_PLATFORM_READ_ADDRESS_COUNT];
    uint8_t log_fifo_probe[NRF_LOG_COLLECTOR_NODE_COUNT];
} nrf_topology_platform_t;


typedef struct {
    nrf_topology_platform_t *platform;
    nrf_topology_snapshot_t *snapshot;
} nrf_cs_platform_context_t;

#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
typedef struct {
    nrf_topology_platform_t *platform;
    nrf_link_readiness_t *links;
    TickType_t last_reconcile_tick;
    TickType_t reconcile_period_ticks;
    uint64_t iteration_started_ms;
    uint64_t node_started_ms;
    uint8_t fifo_probe_done_mask;
} nrf_log_platform_context_t;
#endif

typedef struct {
    nrf_topology_platform_t platform;
    nrf_link_readiness_t links[NRF_LOG_COLLECTOR_NODE_COUNT];
    nrf_log_poll_target_t log_targets[NRF_LOG_COLLECTOR_NODE_COUNT];
    nrf_topology_controller_t controller;
    nrf_sensor_transport_controller_t sensor_controller;
    nrf_topology_snapshot_t snapshot;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_cs_authority_t authority;
    nrf_cs_cycle_snapshot_t cs_cycle_snapshot;
    nrf_cs_platform_context_t cs_context;
    nrf_cs_authority_ops_t cs_ops;
    bool ranging_active;
    uint64_t ranging_deadline_ms;
    uint32_t ranging_topology_generation;
    uint16_t ranging_processed_pairs_mask;
    bool ranging_pair_failure_seen;
    uint8_t ranging_failed_pair_a;
    uint8_t ranging_failed_pair_b;
    nrf_topology_ranging_progress_t ranging_progress;
} nrf_topology_worker_state_t;

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_starting = false;
static bool s_stop_requested = false;
static bool s_topology_refresh_requested = false;
static bool s_snapshot_valid = false;
static bool s_sensor_cache_valid = false;
static uint8_t s_sensor_pending_mask = 0U;
static uint8_t s_sensor_inflight_mask = 0U;
static bool s_height_reference_pending = false;
static uint8_t s_height_reference_pending_mask = 0U;
static uint16_t s_height_reference_pending_generation = 0U;
static bool s_height_reference_in_flight = false;
static uint16_t s_height_reference_in_flight_generation = 0U;
static uint16_t s_height_reference_generation = 0U;
static bool s_imu_calibration_pending = false;
static uint8_t s_imu_calibration_pending_mask = 0U;
static uint16_t s_imu_calibration_pending_generation = 0U;
static nrf_topology_snapshot_t s_snapshot;
static nrf_remote_sensor_cache_t s_sensor_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static nrf_upstream_status_cache_t s_main_status_cache;
static bool s_cs_request_pending = false;
static bool s_cs_request_is_ranging = false;
static uint32_t s_cs_requested_ranging_timeout_ms = 0U;
static uint64_t s_cs_requested_ranging_started_ms = 0U;
static bool s_cs_snapshot_valid = false;
static nrf_cs_policy_t s_cs_requested_policy;
static nrf_cs_cycle_snapshot_t s_cs_cycle_snapshot;
static bool s_ranging_active = false;
static bool s_ranging_progress_valid = false;
static EXT_RAM_BSS_ATTR nrf_topology_ranging_progress_t s_ranging_progress;
static bool s_frozen_ranging_result_valid = false;
static EXT_RAM_BSS_ATTR nrf_topology_ranging_result_t s_frozen_ranging_result;
static nrf_topology_worker_state_t s_worker_state;
static nrf_topology_log_state_t s_topology_log_state;
static bool s_direct_block_log_valid;
static uint32_t s_direct_block_log_ms;
static unsigned s_direct_block_log_conflicts;
static nrf_topology_service_stack_stats_t s_stack_stats;
#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
static nrf_log_collector_t *s_nrf_log_collector;
#endif

static void service_record_stack_hwm(
    nrf_topology_stack_stats_phase_t phase,
    const char *stage,
    uint32_t free_bytes)
{
    const uint8_t phase_bit = (uint8_t)(1U << phase);
    bool first_sample;
    bool new_minimum;
    bool crossed_warning_margin;

    portENTER_CRITICAL(&s_state_lock);
    first_sample = (s_stack_stats.sampled_phase_mask & phase_bit) == 0U;
    new_minimum = first_sample || free_bytes < s_stack_stats.phase_minimum_free_bytes[phase];
    crossed_warning_margin = free_bytes < NRF_TOPOLOGY_STACK_MARGIN_BYTES &&
        (first_sample || s_stack_stats.phase_minimum_free_bytes[phase] >= NRF_TOPOLOGY_STACK_MARGIN_BYTES);
    if (new_minimum) {
        s_stack_stats.phase_minimum_free_bytes[phase] = free_bytes;
    }
    if (s_stack_stats.sampled_phase_mask == 0U || free_bytes < s_stack_stats.minimum_free_bytes) {
        s_stack_stats.minimum_free_bytes = free_bytes;
    }
    s_stack_stats.sampled_phase_mask |= phase_bit;
    portEXIT_CRITICAL(&s_state_lock);

    if (new_minimum) {
        if (crossed_warning_margin) {
            ESP_LOGW(TAG, "STACK_HWM task=nrf_topology stage=%s free=%u margin=%u",
                     stage, (unsigned)free_bytes, (unsigned)NRF_TOPOLOGY_STACK_MARGIN_BYTES);
        } else {
            ESP_LOGI(TAG, "STACK_HWM task=nrf_topology stage=%s free=%u margin=%u",
                     stage, (unsigned)free_bytes, (unsigned)NRF_TOPOLOGY_STACK_MARGIN_BYTES);
        }
    }
}

static void service_record_current_stack_hwm(
    nrf_topology_stack_stats_phase_t phase,
    const char *stage)
{
    service_record_stack_hwm(phase, stage, (uint32_t)uxTaskGetStackHighWaterMark(NULL));
}

static bool service_stop_requested(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

static bool service_register_current_task(void)
{
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    bool stop_before_start;
    portENTER_CRITICAL(&s_state_lock);
    stop_before_start = s_stop_requested;
    s_task = task;
    s_starting = false;
    portEXIT_CRITICAL(&s_state_lock);
    return stop_before_start;
}

static void service_clear_current_task(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    s_sensor_inflight_mask = 0U;
    s_height_reference_pending = false;
    s_height_reference_pending_mask = 0U;
    s_height_reference_pending_generation = 0U;
    s_height_reference_in_flight = false;
    s_height_reference_in_flight_generation = 0U;
    s_imu_calibration_pending = false;
    s_imu_calibration_pending_mask = 0U;
    s_imu_calibration_pending_generation = 0U;
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_publish_state(
    const nrf_topology_snapshot_t *snapshot,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    const nrf_upstream_status_cache_t *main_status_cache)
{
    portENTER_CRITICAL(&s_state_lock);
    s_snapshot = *snapshot;
    memcpy(s_sensor_cache, cache, sizeof(s_sensor_cache));
    if (!snapshot->bus_busy && !snapshot->stale &&
        snapshot->main.state == NRF_TOPOLOGY_NODE_ABSENT) {
        memset(&s_main_status_cache, 0, sizeof(s_main_status_cache));
    }
    else if (main_status_cache != NULL) {
        /* Remote enumeration can yield after a valid direct MAIN STATUS. */
        s_main_status_cache = *main_status_cache;
    }
    s_snapshot_valid = true;
    s_sensor_cache_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_publish_sensor_channel(
    uint8_t channel,
    const nrf_remote_sensor_cache_t *cache,
    const nrf_upstream_status_cache_t *main_status_cache)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sensor_cache[channel] = *cache;
    if (main_status_cache != NULL) {
        s_main_status_cache = *main_status_cache;
    }
    s_sensor_cache_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_queue_sensor_mask(uint8_t mask, bool notify)
{
    TaskHandle_t task;
    portENTER_CRITICAL(&s_state_lock);
    mask &= (uint8_t)~(s_sensor_pending_mask | s_sensor_inflight_mask);
    s_sensor_pending_mask |= mask;
    task = s_task;
    portEXIT_CRITICAL(&s_state_lock);
    if (notify && mask != 0U && task != NULL) {
        (void)xTaskNotifyGive(task);
    }
}

static uint8_t service_take_sensor_mask(void)
{
    uint8_t mask;
    portENTER_CRITICAL(&s_state_lock);
    mask = s_sensor_pending_mask;
    s_sensor_pending_mask = 0U;
    s_sensor_inflight_mask |= mask;
    portEXIT_CRITICAL(&s_state_lock);
    return mask;
}

static void service_finish_sensor_bit(uint8_t bit)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sensor_inflight_mask &= (uint8_t)~bit;
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_requeue_inflight_mask(uint8_t mask)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sensor_inflight_mask &= (uint8_t)~mask;
    s_sensor_pending_mask |= mask;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool service_take_topology_refresh_request(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_topology_refresh_requested;
    s_topology_refresh_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

static void service_request_topology_refresh_from_worker(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_topology_refresh_requested = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool service_topology_refresh_pending(void)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    pending = s_topology_refresh_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static bool service_has_immediate_work(void)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    const bool cs_active = s_ranging_active || s_cs_request_pending ||
        (s_cs_snapshot_valid && s_cs_cycle_snapshot.active);
    const bool sensor_snapshot_usable = s_snapshot_valid &&
        !s_snapshot.bus_busy && !s_snapshot.stale;
    pending = s_cs_request_pending || (!cs_active &&
        (s_topology_refresh_requested ||
         (sensor_snapshot_usable && s_sensor_pending_mask != 0U) ||
         s_height_reference_pending || s_imu_calibration_pending));
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static bool service_background_deferred(
    const nrf_topology_worker_state_t *worker,
    bool cs_initialized)
{
    bool requested_or_published_active;
    portENTER_CRITICAL(&s_state_lock);
    requested_or_published_active = s_cs_request_pending || s_ranging_active ||
        (s_cs_snapshot_valid && s_cs_cycle_snapshot.active);
    portEXIT_CRITICAL(&s_state_lock);
    return requested_or_published_active ||
        (worker != NULL && (worker->ranging_active ||
         (cs_initialized && nrf_cs_authority_is_active(&worker->authority))));
}

static i2c_bus_device_handle_t platform_device_for_address(
    const nrf_topology_platform_t *platform,
    uint8_t address)
{
    if (address == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        return platform->discovery_device;
    }
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        return platform->main_device;
    }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE) {
        return platform->remote_device;
    }
    return NULL;
}

static esp_err_t platform_i2c_read_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)tx;
    (void)tx_len;
    return sensor_bus_read_bytes(
        (i2c_bus_device_handle_t)context, NULL_I2C_MEM_ADDR, rx_len, rx);
}

static esp_err_t platform_i2c_write_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)rx;
    (void)rx_len;
    return sensor_bus_write_bytes(
        (i2c_bus_device_handle_t)context, NULL_I2C_MEM_ADDR, tx_len, tx);
}

static int platform_selected_channel(const nrf_topology_platform_t *platform)
{
    if (platform == NULL) {
        return -1;
    }
    if (platform->selected_mux_channel_valid) {
        return (int)platform->selected_mux_channel;
    }
    return platform->requested_mux_channel_valid
        ? (int)platform->requested_mux_channel : -1;
}

static unsigned platform_snapshot_conflict_count(
    const nrf_topology_snapshot_t *snapshot)
{
    unsigned conflicts = 0U;
    if (snapshot == NULL) {
        return 0U;
    }
    conflicts += snapshot->main.state == NRF_TOPOLOGY_NODE_CONFLICT ? 1U : 0U;
    for (size_t index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        conflicts += snapshot->remotes[index].state == NRF_TOPOLOGY_NODE_CONFLICT
            ? 1U : 0U;
    }
    return conflicts;
}

static unsigned platform_conflict_count(const nrf_topology_platform_t *platform)
{
    unsigned conflicts = platform_snapshot_conflict_count(
        platform != NULL ? platform->active_snapshot : NULL);
    const unsigned last_good_conflicts = platform != NULL &&
        platform->last_good_snapshot_valid
        ? platform_snapshot_conflict_count(&platform->last_good_snapshot) : 0U;
    if (last_good_conflicts > conflicts) {
        conflicts = last_good_conflicts;
    }
    const unsigned blocked_conflicts = platform != NULL &&
        platform->direct_conflict_snapshot_valid
        ? platform_snapshot_conflict_count(&platform->direct_conflict_snapshot) : 0U;
    if (blocked_conflicts > conflicts) {
        conflicts = blocked_conflicts;
    }
    return conflicts;
}

static void platform_capture_direct_conflict_snapshot(
    nrf_topology_platform_t *platform)
{
    if (platform == NULL) {
        return;
    }
    const nrf_topology_snapshot_t *source = platform->active_snapshot;
    if (source == NULL || platform_snapshot_conflict_count(source) == 0U) {
        source = platform->last_good_snapshot_valid
            ? &platform->last_good_snapshot : NULL;
    }
    if ((source == NULL || platform_snapshot_conflict_count(source) == 0U) &&
        platform->direct_conflict_snapshot_valid) {
        source = &platform->direct_conflict_snapshot;
    }
    if (source != NULL && platform_snapshot_conflict_count(source) > 0U) {
        platform->direct_conflict_snapshot = *source;
        platform->direct_conflict_snapshot_valid = true;
    }
}

static bool platform_address_is_nrf(uint8_t address)
{
    return address == NRF_UPSTREAM_ADDRESS_DISCOVERY ||
        address == NRF_UPSTREAM_ADDRESS_MAIN ||
        address == NRF_UPSTREAM_ADDRESS_REMOTE;
}

static bool platform_transfer_is_direct(
    const nrf_topology_platform_t *platform,
    uint8_t address)
{
    if (platform == NULL || platform->selected_mux_channel_valid) {
        return false;
    }
    if (address != NRF_UPSTREAM_ADDRESS_MAIN && platform->mux_bound &&
        platform->requested_mux_channel_valid) {
        return false;
    }
    return true;
}

static void platform_log_direct_nrf_block(
    uint8_t address,
    unsigned conflicts)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    bool emit = false;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_direct_block_log_valid ||
        conflicts != s_direct_block_log_conflicts ||
        (uint32_t)(now_ms - s_direct_block_log_ms) >= 5000U) {
        s_direct_block_log_valid = true;
        s_direct_block_log_ms = now_ms;
        s_direct_block_log_conflicts = conflicts;
        emit = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (emit) {
        ESP_LOGW(TAG,
                 "DIRECT_NRF_IO_BLOCKED address=0x%02X conflicts=%u",
                 (unsigned)address,
                 conflicts);
    }
}

static esp_err_t platform_mux_select(void *context, uint8_t channel);
static esp_err_t platform_close_selected_mux(
    nrf_topology_platform_t *platform,
    bool preserve_requested_channel);
static size_t platform_read_route_index(const nrf_topology_platform_t *platform);
static void platform_record_read_result(
    nrf_topology_platform_t *platform, uint8_t address, size_t route_index, bool success);

static esp_err_t platform_begin_remote_transfer(
    nrf_topology_platform_t *platform,
    uint8_t address)
{
    const unsigned conflicts = platform_conflict_count(platform);
    if (platform != NULL && platform_address_is_nrf(address) &&
        platform_transfer_is_direct(platform, address) && conflicts > 0U) {
        platform_capture_direct_conflict_snapshot(platform);
        platform_log_direct_nrf_block(address, conflicts);
        return SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
    }
    const bool channel_routed_address = address == NRF_UPSTREAM_ADDRESS_REMOTE ||
        address == NRF_UPSTREAM_ADDRESS_DISCOVERY;
    if (!channel_routed_address || platform == NULL || !platform->mux_bound ||
        !platform->requested_mux_channel_valid) {
        return ESP_OK;
    }
    if (platform->bus_owner_held && platform->selected_mux_channel_valid &&
        platform->selected_mux_channel == platform->requested_mux_channel) {
        return ESP_OK;
    }
    return platform_mux_select(platform, platform->requested_mux_channel);
}

static esp_err_t platform_end_remote_transfer(
    nrf_topology_platform_t *platform,
    uint8_t address,
    esp_err_t transfer_err)
{
    const bool channel_routed_address = address == NRF_UPSTREAM_ADDRESS_REMOTE ||
        address == NRF_UPSTREAM_ADDRESS_DISCOVERY;
    if (!channel_routed_address || platform == NULL || !platform->mux_bound ||
        !platform->requested_mux_channel_valid || !platform->bus_owner_held) {
        return transfer_err;
    }
    const esp_err_t close_err = platform_close_selected_mux(platform, true);
    return transfer_err == ESP_OK ? close_err : transfer_err;
}

static esp_err_t platform_i2c_read_bytes(
    nrf_topology_platform_t *platform,
    i2c_bus_device_handle_t device,
    uint8_t address,
    size_t length,
    uint8_t *data)
{
    esp_err_t err = platform_begin_remote_transfer(platform, address);
    if (err != ESP_OK) {
        return err;
    }
    err = sensor_bus_probe_diag_transaction(
        SENSOR_BUS_PROBE_PHASE_READ, address,
        platform_selected_channel(platform), NULL, 0U, data, length,
        false, 0U, platform_i2c_read_transfer, device);
    return platform_end_remote_transfer(platform, address, err);
}

static esp_err_t platform_i2c_write_bytes(
    nrf_topology_platform_t *platform,
    i2c_bus_device_handle_t device,
    uint8_t address,
    size_t length,
    const uint8_t *data)
{
    esp_err_t err = platform_begin_remote_transfer(platform, address);
    if (err != ESP_OK) {
        return err;
    }
    /* Log replies persist across reads; the next ordinary read must reselect. */
    if (platform != NULL && length > 3U && data != NULL &&
        (data[3] == NRF_LOG_MESSAGE_STATUS_REQUEST || data[3] == NRF_LOG_MESSAGE_READ_REQUEST)) {
        platform_record_read_result(platform, address, platform_read_route_index(platform), false);
    }
    err = sensor_bus_probe_diag_transaction(
        SENSOR_BUS_PROBE_PHASE_WRITE, address,
        platform_selected_channel(platform), data, length, NULL, 0U,
        false, 0U, platform_i2c_write_transfer, device);
    return platform_end_remote_transfer(platform, address, err);
}

static esp_err_t platform_normalize_transfer_error(
    nrf_topology_platform_t *platform,
    uint8_t address,
    esp_err_t err);
static esp_err_t platform_recover_classified_error(
    nrf_topology_platform_t *platform,
    uint8_t address,
    esp_err_t err);

static size_t platform_read_address_index(uint8_t address)
{
    if (address == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        return 0U;
    }
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        return 1U;
    }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE) {
        return 2U;
    }
    return NRF_PLATFORM_READ_ADDRESS_COUNT;
}

static size_t platform_read_route_index(const nrf_topology_platform_t *platform)
{
    return platform != NULL && platform->requested_mux_channel_valid
        ? (size_t)platform->requested_mux_channel + 1U
        : 0U;
}

static esp_err_t platform_probe_before_read(
    nrf_topology_platform_t *platform,
    uint8_t address,
    size_t *route_index)
{
    const size_t address_index = platform_read_address_index(address);
    if (platform == NULL || route_index == NULL ||
        address_index >= NRF_PLATFORM_READ_ADDRESS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *route_index = platform_read_route_index(platform);
    if (*route_index >= NRF_PLATFORM_READ_ROUTE_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    if (platform->read_probe_valid[*route_index][address_index]) {
        return ESP_OK;
    }

    esp_err_t probe_err = platform_begin_remote_transfer(platform, address);
    if (probe_err != ESP_OK) {
        return probe_err;
    }
    probe_err = sensor_bus_probe_address_with_context(
        address,
        s_i2c_diag_source,
        platform_selected_channel(platform));
    probe_err = platform_end_remote_transfer(platform, address, probe_err);
    if (probe_err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        probe_err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        return probe_err;
    }
    return probe_err == ESP_OK
        ? ESP_OK
        : platform_normalize_transfer_error(platform, address, probe_err);
}

static esp_err_t platform_probe_status_reselect(
    nrf_topology_platform_t *platform,
    uint8_t address)
{
    if (platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t probe_err = platform_begin_remote_transfer(platform, address);
    if (probe_err != ESP_OK) {
        return probe_err;
    }
    probe_err = sensor_bus_probe_address_with_context(
        address,
        s_i2c_diag_source,
        platform_selected_channel(platform));
    probe_err = platform_end_remote_transfer(platform, address, probe_err);
    if (probe_err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        probe_err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        return probe_err;
    }
    return platform_normalize_transfer_error(platform, address, probe_err);
}

static void platform_record_read_result(
    nrf_topology_platform_t *platform,
    uint8_t address,
    size_t route_index,
    bool success)
{
    const size_t address_index = platform_read_address_index(address);
    if (platform != NULL && route_index < NRF_PLATFORM_READ_ROUTE_COUNT &&
        address_index < NRF_PLATFORM_READ_ADDRESS_COUNT) {
        platform->read_probe_valid[route_index][address_index] = success;
    }
}

static esp_err_t platform_recovery_bus_recover(void *context, i2c_line_result_t *result)
{
    (void)context;
    return sensor_bus_recover(result);
}

static void platform_log_tca_state(nrf_topology_platform_t *platform, bool busy)
{
    const bool faulted = platform != NULL && platform->mux_bound &&
        tca9548a_mux_get_state(&platform->mux) == TCA9548A_MUX_STATE_FAULT;
    const nrf_topology_tca_log_event_t event = nrf_topology_tca_log_update(
        platform != NULL ? &platform->tca_log_state : NULL, faulted, busy,
        busy ? "mutex_busy" : (faulted
            ? tca9548a_mux_fault_reason_name(tca9548a_mux_get_fault_reason(&platform->mux))
            : "successful_close_verify"));
    if (platform == NULL || event == NRF_TOPOLOGY_TCA_LOG_NONE) {
        return;
    }
    if (event == NRF_TOPOLOGY_TCA_LOG_SET) {
        ESP_LOGE(TAG, "TCA_FAULT_SET reason=%s latched_count=%lu",
                 platform->tca_log_state.last_reason,
                 (unsigned long)platform->tca_log_state.latched_count);
    } else if (event == NRF_TOPOLOGY_TCA_LOG_CLEARED) {
        ESP_LOGI(TAG, "TCA_FAULT_CLEARED reason=%s auto_cleared_count=%lu",
                 platform->tca_log_state.last_reason,
                 (unsigned long)platform->tca_log_state.auto_cleared_count);
    } else if (event == NRF_TOPOLOGY_TCA_LOG_BUSY) {
        ESP_LOGW(TAG, "TCA_FAULT_SKIPPED reason=%s", platform->tca_log_state.last_reason);
    }
}

static void platform_log_owner_busy(
    nrf_topology_platform_t *platform,
    const char *source)
{
    sensor_bus_owner_stats_t stats = {0};
    platform_log_tca_state(platform, true);
    if (sensor_bus_get_owner_stats(&stats) != ESP_OK) {
        return;
    }
    ESP_LOGW(TAG,
             "I2C_TIMEOUT_DIAG src=%s addr=0x%02X dev=%s ch=direct err=mutex_busy waited_for_mutex_us=%llu transfer_us=0 holder_task=%s recover_attempts=%lu",
             source,
             platform != NULL ? (unsigned)platform->tca_address : 0U,
             sensor_bus_diag_device_name(
                 platform != NULL ? platform->tca_address : 0U, source),
             (unsigned long long)stats.last_waited_for_mutex_us,
             stats.holder_task[0] != '\0' ? stats.holder_task : "unknown",
             (unsigned long)stats.recover_attempts);
}

static esp_err_t platform_close_selected_mux(
    nrf_topology_platform_t *platform,
    bool preserve_requested_channel)
{
    if (!platform->bus_owner_held &&
        tca9548a_mux_get_state(&platform->mux) == TCA9548A_MUX_STATE_CLOSED) {
        platform->selected_mux_channel_valid = false;
        if (!preserve_requested_channel) {
            platform->requested_mux_channel_valid = false;
        }
        return ESP_OK;
    }
    if (!platform->bus_owner_held) {
        const esp_err_t owner_err = sensor_bus_owner_acquire();
        if (owner_err != ESP_OK) {
            if (owner_err == SENSOR_BUS_ERR_MUTEX_BUSY) {
                platform_log_owner_busy(platform, "tca9548a");
            }
            return owner_err;
        }
        platform->bus_owner_held = true;
        sensor_bus_probe_diag_defer_output();
    }

    esp_err_t err = tca9548a_mux_close(&platform->mux);
    const bool hold_budget_exceeded = sensor_bus_owner_hold_budget_exceeded();
    platform->selected_mux_channel_valid = false;
    platform->bus_owner_held = false;
    sensor_bus_owner_release();
    sensor_bus_probe_diag_flush();
    platform_log_tca_state(platform, false);
    if (!preserve_requested_channel) {
        platform->requested_mux_channel_valid = false;
    }
    if (hold_budget_exceeded && err == ESP_OK) {
        err = SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
    }
    return err;
}

static esp_err_t platform_mux_close_with_owner(
    nrf_topology_platform_t *platform)
{
    return platform_close_selected_mux(platform, false);
}

static void platform_log_reconcile_idle_if_needed(
    nrf_topology_platform_t *platform)
{
    if (platform != NULL && platform->reconcile_pass_active &&
        !platform->reconcile_idle_logged &&
        (!platform->mux_bound ||
         tca9548a_mux_get_state(&platform->mux) ==
             TCA9548A_MUX_STATE_CLOSED)) {
        sensor_bus_probe_diag_log_bus_idle();
        platform->reconcile_idle_logged = true;
    }
}

static esp_err_t platform_recovery_mux_close(void *context)
{
    nrf_topology_platform_t *platform = context;
    if (platform == NULL || !platform->mux_bound) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = platform_close_selected_mux(platform, false);
    sensor_bus_record_transfer_result_with_context(
        platform->tca_address, err, "tca9548a", -1);
    platform->selected_mux_channel_valid = false;
    platform_log_reconcile_idle_if_needed(platform);
    return err;
}

static esp_err_t platform_normalize_transfer_error(
    nrf_topology_platform_t *platform,
    uint8_t address,
    esp_err_t err)
{
    if (err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        return err;
    }
    /* MAX-248: a NACK is ESP_FAIL on the legacy driver but
     * ESP_ERR_INVALID_STATE on i2c_bus v2 / ESP-IDF 5.5 (like a timeout).
     * An absent XIAO must read as ESP_ERR_NOT_FOUND, never as a transport
     * fault, and must not trigger a bus recovery. */
    err = sensor_bus_classify_transfer_error_with_context(
        address,
        err,
        s_i2c_diag_source,
        platform != NULL && platform->selected_mux_channel_valid
            ? (int)platform->selected_mux_channel : -1);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* The IDF 5.5 master uses INVALID_STATE for a timed out transfer.
         * The classifier has already probed and confirmed that the target
         * ACKs, so retain MAX-243 timeout escalation for this bus fault. */
        err = ESP_ERR_TIMEOUT;
    }

    return platform_recover_classified_error(platform, address, err);
}

static esp_err_t platform_recover_classified_error(
    nrf_topology_platform_t *platform,
    uint8_t address,
    esp_err_t err)
{
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        return err;
    }
    if (err == ESP_ERR_TIMEOUT && platform != NULL &&
        platform->reconcile_pass_active) {
        if (platform->timeout_error_pass != platform->reconcile_pass_count) {
            if (platform->timeout_error_pass == 0U ||
                platform->reconcile_pass_count - platform->timeout_error_pass != 1U) {
                platform->timeout_error_windows = 1U;
            } else if (platform->timeout_error_windows < UINT32_MAX) {
                ++platform->timeout_error_windows;
            }
            platform->timeout_error_pass = platform->reconcile_pass_count;
        }
        if (platform->timeout_error_windows < NRF_TOPOLOGY_TIMEOUT_ESCALATION_WINDOWS) {
            return err;
        }
        if (platform->timeout_recovery_pass == platform->reconcile_pass_count) {
            return err;
        }
        platform->timeout_recovery_pass = platform->reconcile_pass_count;
    }
    /* MAX-243: a bus-level failure gets a line check; a stuck SDA is
     * cleared and the mux closed + verified before the next transfer. */
    const nrf_topology_bus_recovery_ops_t ops = {
        .recover = platform_recovery_bus_recover,
        .mux_close = platform_recovery_mux_close,
        .context = platform,
    };
    nrf_topology_bus_recovery_outcome_t outcome = NRF_TOPOLOGY_BUS_NOT_CHECKED;
    const esp_err_t result = nrf_topology_bus_recover_after_error(err, &ops, &outcome);
    service_record_current_stack_hwm(NRF_TOPOLOGY_STACK_PHASE_ERROR_RECOVERY, "error-recovery");
    if (outcome == NRF_TOPOLOGY_BUS_STUCK) {
        if (platform != NULL && platform->mux_bound) {
            tca9548a_mux_latch_bus_stuck(&platform->mux);
            platform_log_tca_state(platform, false);
        }
        ESP_LOGE(TAG,
                 "I2C_DEVICE_FAILURE src=nrf_topology addr=0x%02X dev=%s err=%s state=line_stuck reason=TCA_BUS_STUCK",
                 (unsigned)address,
                 sensor_bus_diag_device_name(address, s_i2c_diag_source),
                 esp_err_to_name(err));
    } else if (outcome == NRF_TOPOLOGY_BUS_RECOVERED_MUX_CLOSED) {
        ESP_LOGW(TAG,
                 "I2C_DEVICE_WARN src=nrf_topology addr=0x%02X dev=%s err=%s state=bus_cleared_mux_closed",
                 (unsigned)address,
                 sensor_bus_diag_device_name(address, s_i2c_diag_source),
                 esp_err_to_name(err));
    } else if (outcome == NRF_TOPOLOGY_BUS_RECOVERED_MUX_FAULT) {
        ESP_LOGE(TAG,
                 "I2C_DEVICE_FAILURE src=nrf_topology addr=0x%02X dev=%s err=%s state=mux_close_readback_failed",
                 (unsigned)address,
                 sensor_bus_diag_device_name(address, s_i2c_diag_source),
                 esp_err_to_name(err));
    }
    return result;
}

static void platform_log_stale_log_response(
    uint8_t address,
    uint8_t type,
    bool reselected)
{
    static uint8_t warning_count;
    if (warning_count >= 4U) {
        return;
    }
    ++warning_count;
    ESP_LOGW(TAG,
             "NRF_STATUS_STALE_LOG_RESPONSE addr=0x%02X type=0x%02X reselect=%s",
             (unsigned)address,
             (unsigned)type,
             reselected ? "ok" : "fail");
}

static void platform_store_main_status(
    nrf_topology_platform_t *platform,
    const nrf_upstream_status_t *status)
{
    nrf_upstream_status_cache_t *cache = &platform->main_status_cache;
    memset(cache, 0, sizeof(*cache));
    cache->valid = true;
    const bool wire_identity_valid = status->hardware_id_length > 0U &&
        status->hardware_id_length <= NRF_UPSTREAM_PROTOCOL_HW_ID_MAX;
    if (platform->expected_main_identity_valid) {
        const nrf_topology_node_snapshot_t *expected = &platform->expected_main_identity;
        cache->identity = *expected;
        cache->identity_matches = wire_identity_valid &&
            status->hardware_id_length == expected->hardware_id_length &&
            memcmp(status->hardware_id, expected->hardware_id, status->hardware_id_length) == 0 &&
            status->request_id == expected->assignment_request_id &&
            status->role == NRF_UPSTREAM_ROLE_MAIN &&
            status->node_id == expected->node_id &&
            status->mux_channel == expected->mux_channel &&
            status->target_address == expected->current_address;
    }
    else {
        cache->identity_matches = wire_identity_valid &&
            status->role == NRF_UPSTREAM_ROLE_MAIN &&
            status->node_id == NRF_TOPOLOGY_MAIN_NODE_ID &&
            status->mux_channel == NRF_UPSTREAM_MUX_CHANNEL_NONE &&
            status->target_address == NRF_UPSTREAM_ADDRESS_MAIN;
        cache->identity.state = NRF_TOPOLOGY_NODE_ASSIGNED;
        cache->identity.hardware_id_length = status->hardware_id_length;
        if (wire_identity_valid) {
            memcpy(cache->identity.hardware_id, status->hardware_id, status->hardware_id_length);
        }
        cache->identity.node_id = status->node_id;
        cache->identity.mux_channel = status->mux_channel;
        cache->identity.current_address = status->target_address;
        cache->identity.assignment_request_id = status->request_id;
    }
    cache->lifecycle = status->lifecycle;
    cache->result = status->result;
    cache->flags = status->flags;
    const int64_t now_us = esp_timer_get_time();
    cache->esp_receipt_uptime_ms = now_us >= 0 ? (uint64_t)now_us / 1000U : 0U;
}

static esp_err_t platform_read_status(
    void *context,
    uint8_t address,
    nrf_upstream_status_t *status)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH];
    i2c_bus_device_handle_t device;
    esp_err_t err;

    if (platform == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device = platform_device_for_address(platform, address);
    if (device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t route_index = 0U;
    err = platform_probe_before_read(platform, address, &route_index);
    if (err != ESP_OK) {
        return err;
    }

    err = platform_i2c_read_bytes(
        platform, device, address, sizeof(frame), frame);
    if (err != ESP_OK) {
        platform_record_read_result(platform, address, route_index, false);
    } else {
        platform_record_read_result(platform, address, route_index, true);
    }
    err = platform_normalize_transfer_error(platform, address, err);
    if (err != ESP_OK) {
        return err;
    }

    if (nrf_upstream_protocol_decode_status(frame, sizeof(frame), status) == NRF_UPSTREAM_RESULT_OK) {
        if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
            platform_store_main_status(platform, status);
        }
        return ESP_OK;
    }
    if (sizeof(frame) < 4U || frame[0] != 'G' || frame[1] != 'S' ||
        (frame[3] != NRF_LOG_MESSAGE_STATUS && frame[3] != NRF_LOG_MESSAGE_BLOCK)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t stale_type = frame[3];
    err = platform_probe_status_reselect(platform, address);
    if (err != ESP_OK) {
        platform_log_stale_log_response(address, stale_type, false);
        return err;
    }
    platform_log_stale_log_response(address, stale_type, true);

    err = platform_i2c_read_bytes(
        platform, device, address, sizeof(frame), frame);
    platform_record_read_result(platform, address, route_index, err == ESP_OK);
    err = platform_normalize_transfer_error(platform, address, err);
    if (err != ESP_OK) {
        return err;
    }
    if (nrf_upstream_protocol_decode_status(frame, sizeof(frame), status) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        platform_store_main_status(platform, status);
    }
    return ESP_OK;
}

static esp_err_t platform_write_assign(
    void *context,
    uint8_t address,
    const nrf_upstream_assign_t *assign)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
    i2c_bus_device_handle_t device;
    esp_err_t err;

    if (platform == NULL || assign == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device = platform_device_for_address(platform, address);
    if (device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nrf_upstream_protocol_encode_assign(assign, frame) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    err = platform_i2c_write_bytes(
        platform, device, address, sizeof(frame), frame);
    return platform_normalize_transfer_error(platform, address, err);
}

static esp_err_t platform_write_sensor_refresh(
    void *context,
    const nrf_upstream_sensor_refresh_t *request)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH];
    esp_err_t err;

    if (platform == NULL || request == NULL || platform->remote_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nrf_upstream_protocol_encode_sensor_refresh(request, frame) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = platform_i2c_write_bytes(
        platform, platform->remote_device, NRF_UPSTREAM_ADDRESS_REMOTE,
        sizeof(frame), frame);
    return platform_normalize_transfer_error(
        platform, NRF_UPSTREAM_ADDRESS_REMOTE, err);
}

static esp_err_t platform_write_height_reference(
    void *context,
    const nrf_upstream_height_reference_t *request)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH];
    esp_err_t err;

    if (platform == NULL || request == NULL || platform->remote_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nrf_upstream_protocol_encode_height_reference(request, frame) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = platform_i2c_write_bytes(
        platform, platform->remote_device, NRF_UPSTREAM_ADDRESS_REMOTE,
        sizeof(frame), frame);
    return platform_normalize_transfer_error(
        platform, NRF_UPSTREAM_ADDRESS_REMOTE, err);
}

static esp_err_t platform_write_imu_calibration(
    void *context,
    const nrf_upstream_imu_calibrate_t *request)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH];
    esp_err_t err;

    if (platform == NULL || request == NULL || platform->remote_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nrf_upstream_protocol_encode_imu_calibrate(request, frame) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = platform_i2c_write_bytes(
        platform, platform->remote_device, NRF_UPSTREAM_ADDRESS_REMOTE,
        sizeof(frame), frame);
    return platform_normalize_transfer_error(
        platform, NRF_UPSTREAM_ADDRESS_REMOTE, err);
}

static esp_err_t platform_read_remote_snapshot(
    void *context,
    nrf_upstream_status_t *status,
    nrf_upstream_sensor_snapshot_t *sensor)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH];
    esp_err_t err;
    size_t route_index = 0U;

    if (platform == NULL || status == NULL || sensor == NULL || platform->remote_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = platform_probe_before_read(
        platform, NRF_UPSTREAM_ADDRESS_REMOTE, &route_index);
    if (err != ESP_OK) {
        return err;
    }
    err = platform_i2c_read_bytes(
        platform, platform->remote_device, NRF_UPSTREAM_ADDRESS_REMOTE,
        sizeof(frame), frame);
    if (err != ESP_OK) {
        platform_record_read_result(
            platform, NRF_UPSTREAM_ADDRESS_REMOTE, route_index, false);
    } else {
        platform_record_read_result(
            platform, NRF_UPSTREAM_ADDRESS_REMOTE, route_index, true);
    }
    err = platform_normalize_transfer_error(
        platform, NRF_UPSTREAM_ADDRESS_REMOTE, err);
    if (err != ESP_OK) {
        return err;
    }
    const size_t frame_length = (size_t)frame[32] | ((size_t)frame[33] << 8U);
    if (frame_length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH &&
        frame_length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V2_LENGTH &&
        frame_length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return nrf_upstream_protocol_decode_remote_snapshot(frame, frame_length, status, sensor) == NRF_UPSTREAM_RESULT_OK
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t platform_mux_close(void *context);
static esp_err_t platform_mux_select(void *context, uint8_t channel);
static uint64_t platform_now_ms(void *context);
static bool tick_period_due(TickType_t now, TickType_t last, TickType_t period);

#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
static esp_err_t platform_log_begin_node(void *context, uint8_t node_index)
{
    nrf_log_platform_context_t *log_context = context;
    if (log_context == NULL || log_context->platform == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    log_context->node_started_ms = platform_now_ms(NULL);
    if (node_index == 0U) return ESP_OK;
    const esp_err_t err = platform_mux_select(log_context->platform, (uint8_t)(node_index - 1U));
    if (err != ESP_OK) service_request_topology_refresh_from_worker();
    return err;
}

static esp_err_t platform_log_end_node(void *context, uint8_t node_index)
{
    nrf_log_platform_context_t *log_context = context;
    if (log_context == NULL || log_context->platform == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (node_index == 0U) return ESP_OK;
    const esp_err_t err = platform_mux_close(log_context->platform);
    if (err != ESP_OK) service_request_topology_refresh_from_worker();
    return err;
}

static uint64_t platform_log_now_ms(void *context);

static esp_err_t platform_log_handle_transfer_error(
    const nrf_log_platform_context_t *log_context,
    uint8_t node_index,
    uint8_t address,
    esp_err_t err)
{
    if (err == ESP_OK) return ESP_OK;
    if (err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        return err;
    }
    if (log_context == NULL) return ESP_ERR_INVALID_ARG;
    if (log_context->links == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return platform_normalize_transfer_error(log_context->platform, address, err);
    }
    nrf_link_readiness_t *link = &log_context->links[node_index];
    const uint64_t now_ms = platform_log_now_ms((void *)log_context);
    nrf_link_readiness_record_failure(link, now_ms);
    if (nrf_link_readiness_classification_due(link, now_ms)) {
        nrf_link_readiness_classification_done(link);
        return platform_normalize_transfer_error(log_context->platform, address, err);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t platform_log_write_frame(
    void *context,
    uint8_t node_index,
    const uint8_t *frame,
    size_t frame_length)
{
    nrf_log_platform_context_t *log_context = context;
    const uint8_t address = node_index == 0U
        ? NRF_UPSTREAM_ADDRESS_MAIN
        : NRF_UPSTREAM_ADDRESS_REMOTE;
    i2c_bus_device_handle_t device;
    esp_err_t err;
    if (log_context == NULL || log_context->platform == NULL || frame == NULL ||
        node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    device = platform_device_for_address(log_context->platform, address);
    if (device == NULL) return ESP_ERR_INVALID_STATE;
    err = platform_i2c_write_bytes(
        log_context->platform, device, address, frame_length, frame);
    err = platform_log_handle_transfer_error(log_context, node_index, address, err);
    if (err == ESP_ERR_NOT_FOUND) service_request_topology_refresh_from_worker();
    return err;
}

static esp_err_t platform_log_read_once(
    void *context,
    uint8_t node_index,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *received_length)
{
    nrf_log_platform_context_t *log_context = context;
    const uint8_t address = node_index == 0U
        ? NRF_UPSTREAM_ADDRESS_MAIN
        : NRF_UPSTREAM_ADDRESS_REMOTE;
    i2c_bus_device_handle_t device;
    esp_err_t err;
    size_t route_index = 0U;
    if (received_length != NULL) *received_length = 0U;
    if (log_context == NULL || log_context->platform == NULL || frame == NULL ||
        received_length == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT ||
        frame_capacity == 0U || frame_capacity > NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    device = platform_device_for_address(log_context->platform, address);
    if (device == NULL) return ESP_ERR_INVALID_STATE;
    /* An address-only write probe would discard the selected log response. */
    route_index = platform_read_route_index(log_context->platform);
    const int64_t started_us = esp_timer_get_time();
    err = platform_i2c_read_bytes(
        log_context->platform, device, address, frame_capacity, frame);
    const int64_t call_us = esp_timer_get_time() - started_us;
    char response_type[5] = "n/a";
    int declared_length = -1;
    if (err == ESP_OK && frame_capacity >= 6U) {
        (void)snprintf(response_type, sizeof(response_type), "0x%02x", (unsigned)frame[3]);
        declared_length = (int)((unsigned)frame[4] | ((unsigned)frame[5] << 8U));
    }
    ESP_LOGI(TAG,
             "NRF_LOG_RX node=%u addr=0x%02x ch=%d requested=%u received=%d "
             "type=%s declared=%d call_us=%" PRId64 " err=0x%x(%s)",
             (unsigned)node_index, (unsigned)address,
             platform_selected_channel(log_context->platform), (unsigned)frame_capacity,
             err == ESP_OK ? (int)frame_capacity : -1, response_type, declared_length,
             call_us, (unsigned)err, esp_err_to_name(err));
    platform_record_read_result(log_context->platform, address, route_index, false);
    err = platform_log_handle_transfer_error(log_context, node_index, address, err);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) service_request_topology_refresh_from_worker();
        return err;
    }
    *received_length = frame_capacity;
    return ESP_OK;
}

static bool platform_log_yield_requested(void *context);

static esp_err_t platform_log_read_frame(
    void *context,
    uint8_t node_index,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *received_length)
{
    nrf_log_platform_context_t *log_context = context;
    if (frame_capacity != NRF_LOG_PROTOCOL_STATUS_LENGTH) {
        return platform_log_read_once(context, node_index, frame, frame_capacity, received_length);
    }
    if (received_length != NULL) *received_length = 0U;
    if (log_context == NULL || log_context->platform == NULL ||
        node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    const uint64_t now_ms = platform_now_ms(NULL);
    const uint32_t read_reserve_ms = 2U * SENSOR_BUS_TRANSACTION_TIMEOUT_MS;
    const uint8_t node_mask = (uint8_t)(1U << node_index);
    const bool probe_allowed =
        (log_context->fifo_probe_done_mask & node_mask) == 0U &&
        now_ms - log_context->node_started_ms + read_reserve_ms <= CONFIG_NRF_LOG_POLL_NODE_BUDGET_MS &&
        now_ms - log_context->iteration_started_ms + read_reserve_ms <= CONFIG_NRF_LOG_POLL_ITERATION_BUDGET_MS &&
        !platform_log_yield_requested(context);
    if (probe_allowed) log_context->fifo_probe_done_mask |= node_mask;
    const nrf_log_collector_io_t io = {
        .context = context,
        .read_frame = platform_log_read_once,
    };
    const esp_err_t err = nrf_log_collector_read_status_with_probe(
        &io, node_index, &log_context->platform->log_fifo_probe[node_index],
        probe_allowed, frame, received_length);
    if (!probe_allowed || err == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGI(TAG, "NRF_FIFO_CHECK node=%u extra_allowed=%u next_step=%u err=0x%x(%s)",
                 (unsigned)node_index, (unsigned)probe_allowed,
                 (unsigned)log_context->platform->log_fifo_probe[node_index],
                 (unsigned)err, esp_err_to_name(err));
    }
    return err;
}

static uint64_t platform_log_now_ms(void *context)
{
    (void)context;
    return platform_now_ms(NULL);
}

static bool platform_log_yield_requested(void *context)
{
    const nrf_log_platform_context_t *log_context = context;
    if (log_context == NULL) return true;
    return service_stop_requested() || service_has_immediate_work() ||
           tick_period_due(
               xTaskGetTickCount(),
               log_context->last_reconcile_tick,
               log_context->reconcile_period_ticks);
}

static bool platform_log_node_link_ready(void *context, uint8_t node_index)
{
    const nrf_log_platform_context_t *log_context = context;
    if (log_context == NULL || log_context->links == NULL ||
        node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return false;
    }
    return nrf_link_readiness_is_ready(&log_context->links[node_index]);
}

static void platform_log_report_link_failure(void *context, uint8_t node_index)
{
    const nrf_log_platform_context_t *log_context = context;
    if (log_context == NULL || log_context->links == NULL ||
        node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return;
    }
    nrf_link_readiness_record_failure(
        &log_context->links[node_index], platform_log_now_ms(context));
}

static void service_log_fill_target(
    nrf_log_poll_target_t *target,
    const nrf_topology_node_snapshot_t *node,
    bool route_available,
    bool link_ready)
{
    memset(target, 0, sizeof(*target));
    target->present = node->state == NRF_TOPOLOGY_NODE_ASSIGNED;
    target->route_available = target->present && route_available;
    target->link_ready = target->present && link_ready;
    target->node_id = node->node_id;
    target->mux_channel = node->mux_channel;
    target->current_address = node->current_address;
    target->hardware_id_length = node->hardware_id_length;
    if (target->hardware_id_length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) {
        target->hardware_id_length = NRF_UPSTREAM_PROTOCOL_HW_ID_MAX;
    }
    memcpy(target->hardware_id, node->hardware_id, target->hardware_id_length);
    target->assignment_generation = node->assignment_request_id;
}

static void service_log_build_targets(
    nrf_topology_worker_state_t *worker,
    nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT])
{
    service_log_fill_target(
        &targets[0],
        &worker->snapshot.main,
        true,
        nrf_link_readiness_is_ready(&worker->links[0]));
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        service_log_fill_target(
            &targets[channel + 1U],
            &worker->snapshot.remotes[channel],
            worker->snapshot.tca_available && !worker->snapshot.tca_fault,
            nrf_link_readiness_is_ready(&worker->links[channel + 1U]));
    }
    if (worker->authority.command_active) {
        const uint8_t initiator = worker->authority.request.step.initiator;
        const uint8_t reflector = worker->authority.request.step.reflector;
        for (uint8_t i = 0U; i < NRF_LOG_COLLECTOR_NODE_COUNT; ++i) {
            /* Skip both nodes while command_active spans CS EXECUTE through its RESULT read. */
            targets[i].cs_busy = targets[i].present &&
                (targets[i].node_id == initiator || targets[i].node_id == reflector);
        }
    }
}

static void service_log_sync_targets(nrf_topology_worker_state_t *worker)
{
    nrf_log_poll_target_t *targets = worker->log_targets;
    if (s_nrf_log_collector == NULL) return;
    service_log_build_targets(worker, targets);
    (void)nrf_log_collector_sync_targets(s_nrf_log_collector, targets);
}

static void service_log_collect_iteration(
    nrf_topology_worker_state_t *worker,
    TickType_t last_reconcile_tick,
    TickType_t reconcile_period_ticks)
{
    nrf_log_poll_target_t *targets = worker->log_targets;
    nrf_log_collector_iteration_t iteration;
    nrf_log_platform_context_t context = {
        .platform = &worker->platform,
        .links = worker->links,
        .last_reconcile_tick = last_reconcile_tick,
        .reconcile_period_ticks = reconcile_period_ticks,
        .iteration_started_ms = platform_now_ms(NULL),
    };
    const nrf_log_collector_io_t io = {
        .context = &context,
        .begin_node = platform_log_begin_node,
        .end_node = platform_log_end_node,
        .write_frame = platform_log_write_frame,
        .read_frame = platform_log_read_frame,
        .now_ms = platform_log_now_ms,
        .yield_requested = platform_log_yield_requested,
        .node_link_ready = platform_log_node_link_ready,
        .report_link_failure = platform_log_report_link_failure,
    };
    if (s_nrf_log_collector == NULL) return;
    service_log_build_targets(worker, targets);
    if (nrf_log_collector_sync_targets(s_nrf_log_collector, targets) != ESP_OK) return;
    (void)nrf_log_collector_poll_iteration(
        s_nrf_log_collector,
        targets,
        &io,
        CONFIG_NRF_LOG_POLL_NODE_BUDGET_MS,
        CONFIG_NRF_LOG_POLL_ITERATION_BUDGET_MS,
        &iteration);
    service_record_current_stack_hwm(
        NRF_TOPOLOGY_STACK_PHASE_LOG_COLLECTOR,
        "log-collector");
}
#endif

static esp_err_t platform_mux_close(void *context)
{
    nrf_topology_platform_t *platform = context;
    if (platform == NULL || !platform->mux_bound) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = platform_close_selected_mux(platform, false);
    sensor_bus_record_transfer_result_with_context(
        platform->tca_address, err, "tca9548a", -1);
    platform->selected_mux_channel_valid = false;
    platform_log_reconcile_idle_if_needed(platform);
    return err;
}

static esp_err_t platform_mux_select(void *context, uint8_t channel)
{
    nrf_topology_platform_t *platform = context;
    if (platform == NULL || !platform->mux_bound) {
        return ESP_ERR_INVALID_STATE;
    }
    platform->requested_mux_channel = channel;
    platform->requested_mux_channel_valid = true;
    if (!platform->bus_owner_held) {
        const esp_err_t owner_err = sensor_bus_owner_acquire();
        if (owner_err != ESP_OK) {
            if (owner_err == SENSOR_BUS_ERR_MUTEX_BUSY) {
                platform_log_owner_busy(platform, "tca9548a");
            }
            platform->requested_mux_channel_valid = false;
            return owner_err;
        }
        platform->bus_owner_held = true;
        sensor_bus_probe_diag_defer_output();
    }
    const esp_err_t err = tca9548a_mux_select(&platform->mux, channel);
    if (err == ESP_OK) {
        platform->selected_mux_channel = channel;
        platform->selected_mux_channel_valid = true;
    } else {
        platform->selected_mux_channel_valid = false;
        platform->requested_mux_channel_valid = false;
        platform->bus_owner_held = false;
        sensor_bus_owner_release();
        sensor_bus_probe_diag_flush();
        platform_log_tca_state(platform, false);
    }
    sensor_bus_record_transfer_result_with_context(
        platform->tca_address, err, "tca9548a", -1);
    return err;
}

static bool platform_mux_faulted(void *context)
{
    nrf_topology_platform_t *platform = context;
    return platform == NULL || !platform->mux_bound ||
           tca9548a_mux_get_state(&platform->mux) == TCA9548A_MUX_STATE_FAULT;
}

static void platform_sleep_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static uint64_t platform_now_ms(void *context)
{
    const int64_t now_us = esp_timer_get_time();
    (void)context;
    return now_us > 0 ? (uint64_t)now_us / 1000U : 0U;
}

static bool platform_abort_requested(void *context)
{
    (void)context;
    return service_stop_requested();
}

static void platform_delete_device(i2c_bus_device_handle_t *device)
{
    if (device != NULL && *device != NULL) {
        (void)sensor_bus_device_delete(device);
    }
}

static void platform_close(nrf_topology_platform_t *platform)
{
    if (platform == NULL) {
        return;
    }
    if (platform->mux_bound) {
        const esp_err_t close_err = platform_mux_close_with_owner(platform);
        sensor_bus_record_transfer_result_with_context(
            platform->tca_address, close_err, "tca9548a", -1);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "I2C_DEVICE_WARN src=tca9548a addr=0x%02X dev=%s reason=final_close_failed err=%s",
                     (unsigned)platform->tca_address,
                     sensor_bus_diag_device_name(
                         platform->tca_address, "tca9548a"),
                     esp_err_to_name(close_err));
        }
    }
    platform->selected_mux_channel_valid = false;
    memset(platform->read_probe_valid, 0, sizeof(platform->read_probe_valid));
    platform_delete_device(&platform->remote_device);
    platform_delete_device(&platform->main_device);
    platform_delete_device(&platform->discovery_device);
    platform->tca_device = NULL;
    platform->mux_bound = false;
    memset(&platform->bus_handles, 0, sizeof(platform->bus_handles));
}

static esp_err_t platform_open_address_handles(nrf_topology_platform_t *platform)
{
    esp_err_t err;
    if (platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (platform->discovery_device != NULL &&
        platform->main_device != NULL &&
        platform->remote_device != NULL) {
        return ESP_OK;
    }

    err = sensor_bus_get_handles(&platform->bus_handles);
    if (err != ESP_OK) {
        return err;
    }

    if (platform->discovery_device == NULL) {
        err = sensor_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_DISCOVERY,
            SENSOR_BUS_I2C_SPEED_HZ,
            &platform->discovery_device);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (platform->main_device == NULL) {
        err = sensor_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_MAIN,
            SENSOR_BUS_I2C_SPEED_HZ,
            &platform->main_device);
        if (err != ESP_OK) {
            platform_delete_device(&platform->discovery_device);
            return err;
        }
    }
    if (platform->remote_device == NULL) {
        err = sensor_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_REMOTE,
            SENSOR_BUS_I2C_SPEED_HZ,
            &platform->remote_device);
        if (err != ESP_OK) {
            platform_delete_device(&platform->main_device);
            platform_delete_device(&platform->discovery_device);
            return err;
        }
    }
    return ESP_OK;
}

static void platform_prepare_tca(
    nrf_topology_platform_t *platform,
    bool *available,
    bool *fault)
{
    esp_err_t err;
    i2c_bus_device_handle_t tca = NULL;
    uint8_t address = NULL_I2C_DEV_ADDR;

    *available = false;
    *fault = false;
    if (platform->mux_bound) {
        *available = true;
        return;
    }

    err = sensor_bus_get_tca9548a(&tca, &address);
    if (err == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        *fault = true;
        if (err == ESP_ERR_TIMEOUT) {
            /* Mux discovery must participate in the same bounded recovery as node reads. */
            (void)platform_recover_classified_error(platform, address, err);
        }
        return;
    }

    err = tca9548a_mux_bind_i2c_bus(&platform->mux, tca, address);
    if (err != ESP_OK) {
        *fault = true;
        return;
    }
    platform->tca_device = tca;
    platform->tca_address = address;
    platform->mux_bound = true;
    *available = true;
    ESP_LOGI(TAG,
             "I2C_DEVICE_INFO src=tca9548a addr=0x%02X dev=%s state=mux_available",
             (unsigned)address,
             sensor_bus_diag_device_name(address, "tca9548a"));
}

static void service_log_snapshot(const nrf_topology_snapshot_t *snapshot)
{
    unsigned assigned = snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    unsigned conflicts = snapshot->main.state == NRF_TOPOLOGY_NODE_CONFLICT ? 1U : 0U;
    uint32_t now_ms;

    for (size_t index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        assigned += snapshot->remotes[index].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
        conflicts += snapshot->remotes[index].state == NRF_TOPOLOGY_NODE_CONFLICT ? 1U : 0U;
    }
    now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (!nrf_topology_log_should_emit(
            &s_topology_log_state,
            now_ms,
            snapshot->generation,
            snapshot->tca_available,
            snapshot->tca_fault,
            assigned,
            conflicts,
            (int32_t)snapshot->main.state
        )) {
        return;
    }

    ESP_LOGI(
        TAG,
        "generation=%u tca=%s%s assigned=%u conflicts=%u main_state=%d bus_busy=%u stale=%u age_ms=%lu",
        (unsigned)snapshot->generation,
        snapshot->tca_available ? "yes" : "no",
        snapshot->tca_fault ? "/fault" : "",
        assigned,
        conflicts,
        (int)snapshot->main.state,
        snapshot->bus_busy ? 1U : 0U,
        snapshot->stale ? 1U : 0U,
        (unsigned long)snapshot->age_ms);
}

static void service_snapshot_set_nodes_unknown(nrf_topology_snapshot_t *snapshot)
{
    memset(&snapshot->main, 0, sizeof(snapshot->main));
    snapshot->main.state = NRF_TOPOLOGY_NODE_UNKNOWN;
    for (size_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        memset(&snapshot->remotes[channel], 0, sizeof(snapshot->remotes[channel]));
        snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_UNKNOWN;
    }
}

static void service_apply_snapshot_freshness(
    nrf_topology_platform_t *platform,
    nrf_topology_snapshot_t *snapshot)
{
    const uint64_t now_ms = platform_now_ms(platform);
    if (!snapshot->bus_busy) {
        snapshot->stale = false;
        snapshot->age_ms = 0U;
        platform->last_good_snapshot = *snapshot;
        platform->last_good_snapshot_valid = true;
        platform->last_good_snapshot_ms = now_ms;
        if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            platform->expected_main_identity = snapshot->main;
            platform->expected_main_identity_valid = true;
        }
        return;
    }

    const uint32_t attempted_generation = snapshot->generation;
    if (!platform->last_good_snapshot_valid) {
        snapshot->stale = true;
        snapshot->age_ms = 0U;
        service_snapshot_set_nodes_unknown(snapshot);
        return;
    }

    *snapshot = platform->last_good_snapshot;
    snapshot->generation = attempted_generation;
    snapshot->bus_busy = true;
    snapshot->stale = true;
    const uint64_t age_ms = now_ms >= platform->last_good_snapshot_ms
        ? now_ms - platform->last_good_snapshot_ms : 0U;
    snapshot->age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    if (age_ms >= NRF_TOPOLOGY_STALE_LIMIT_MS) {
        service_snapshot_set_nodes_unknown(snapshot);
    }
}

static void service_preserve_direct_conflicts(
    const nrf_topology_platform_t *platform,
    nrf_topology_snapshot_t *snapshot)
{
    if (platform == NULL || snapshot == NULL ||
        !platform->direct_conflict_snapshot_valid) {
        return;
    }
    if (platform->direct_conflict_snapshot.main.state == NRF_TOPOLOGY_NODE_CONFLICT) {
        snapshot->main = platform->direct_conflict_snapshot.main;
    } else if (!platform->last_good_snapshot_valid &&
               snapshot->main.state == NRF_TOPOLOGY_NODE_UNKNOWN &&
               platform->direct_conflict_snapshot.main.state ==
                   NRF_TOPOLOGY_NODE_ASSIGNED) {
        snapshot->main = platform->direct_conflict_snapshot.main;
    }
    for (size_t index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        if (platform->direct_conflict_snapshot.remotes[index].state ==
            NRF_TOPOLOGY_NODE_CONFLICT) {
            snapshot->remotes[index] = platform->direct_conflict_snapshot.remotes[index];
        }
    }
}

static bool tick_period_due(TickType_t now, TickType_t last, TickType_t period)
{
    return (TickType_t)(now - last) >= period;
}

static TickType_t tick_wait_until_period(TickType_t now, TickType_t last, TickType_t period)
{
    const TickType_t elapsed = (TickType_t)(now - last);
    return elapsed >= period ? 0U : (TickType_t)(period - elapsed);
}

static bool service_run_topology_reconcile(
    nrf_topology_platform_t *platform,
    nrf_topology_controller_t *controller,
    nrf_topology_snapshot_t *snapshot,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT])
{
    if (platform->reconcile_pass_count < UINT32_MAX) {
        platform->reconcile_pass_count++;
    }
    sensor_bus_probe_diag_begin_pass(platform->reconcile_pass_count);
    platform->reconcile_pass_active = true;
    platform->reconcile_idle_logged = false;
    platform->active_snapshot = snapshot;

    const esp_err_t open_err = platform_open_address_handles(platform);
    esp_err_t reconcile_err;

    if (open_err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        open_err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        ++controller->generation;
        if (controller->generation == 0U) {
            controller->generation = 1U;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->generation = controller->generation;
        snapshot->bus_busy = true;
        reconcile_err = ESP_OK;
    } else if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "nRF topology address handles unavailable: %s", esp_err_to_name(open_err));
        platform_log_reconcile_idle_if_needed(platform);
        const nrf_topology_io_t failed_io = {
            .context = platform,
            .tca_available = false,
            .tca_fault = true,
            .mux_close = platform_mux_close,
            .mux_select = platform_mux_select,
            .read_status = platform_read_status,
            .write_assign = platform_write_assign,
            .now_ms = platform_now_ms,
            .sleep_ms = platform_sleep_ms,
            .abort_requested = platform_abort_requested,
        };
        reconcile_err = nrf_topology_reconcile(controller, &failed_io, snapshot);
    }
    else {
        bool tca_available;
        bool tca_fault;
        platform_prepare_tca(platform, &tca_available, &tca_fault);
        if (!tca_available || tca_fault) {
            platform_log_reconcile_idle_if_needed(platform);
        }
        const nrf_topology_io_t io = {
            .context = platform,
            .tca_available = tca_available,
            .tca_fault = tca_fault,
            .mux_close = platform_mux_close,
            .mux_select = platform_mux_select,
            .read_status = platform_read_status,
            .write_assign = platform_write_assign,
            .now_ms = platform_now_ms,
            .sleep_ms = platform_sleep_ms,
            .abort_requested = platform_abort_requested,
        };
        reconcile_err = nrf_topology_reconcile(controller, &io, snapshot);
    }

    sensor_bus_probe_diag_end_pass();
    platform->reconcile_pass_active = false;

    if (reconcile_err != ESP_OK) {
        platform->active_snapshot = NULL;
        return false;
    }

    service_apply_snapshot_freshness(platform, snapshot);
    service_preserve_direct_conflicts(platform, snapshot);
    platform->active_snapshot = NULL;
    const uint8_t initial_mask = nrf_sensor_cache_reconcile_topology(snapshot, cache);
    service_publish_state(snapshot, cache, &platform->main_status_cache);
    service_queue_sensor_mask(initial_mask, false);
    service_log_snapshot(snapshot);
    return true;
}

static void service_process_sensor_mask(
    nrf_topology_platform_t *platform,
    nrf_sensor_transport_controller_t *sensor_controller,
    const nrf_topology_snapshot_t *snapshot,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    uint8_t mask,
    TickType_t last_reconcile_tick,
    TickType_t reconcile_period_ticks)
{
    const nrf_sensor_transport_io_t io = {
        .context = platform,
        .mux_select = platform_mux_select,
        .mux_close = platform_mux_close,
        .mux_faulted = platform_mux_faulted,
        .read_status = platform_read_status,
        .write_sensor_refresh = platform_write_sensor_refresh,
        .write_height_reference = platform_write_height_reference,
        .read_remote_snapshot = platform_read_remote_snapshot,
        .sleep_ms = platform_sleep_ms,
        .now_ms = platform_now_ms,
        .abort_requested = platform_abort_requested,
    };

    if (snapshot->bus_busy || snapshot->stale) {
        service_requeue_inflight_mask(mask);
        return;
    }

    while (mask != 0U && !service_stop_requested()) {
        const TickType_t now = xTaskGetTickCount();
        if (service_topology_refresh_pending() ||
            tick_period_due(now, last_reconcile_tick, reconcile_period_ticks)) {
            service_requeue_inflight_mask(mask);
            return;
        }

        uint8_t channel = 0U;
        while (channel < NRF_TOPOLOGY_REMOTE_COUNT && (mask & (uint8_t)(1U << channel)) == 0U) {
            ++channel;
        }
        if (channel >= NRF_TOPOLOGY_REMOTE_COUNT) {
            break;
        }

        const uint8_t bit = (uint8_t)(1U << channel);
        mask &= (uint8_t)~bit;

        if (snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED &&
            snapshot->tca_available && !snapshot->tca_fault) {
            bool request_reconcile = false;
            (void)nrf_sensor_transport_refresh_channel(
                sensor_controller,
                &io,
                snapshot,
                channel,
                &cache[channel],
                &request_reconcile);
            service_publish_sensor_channel(
                channel,
                &cache[channel],
                &platform->main_status_cache);
            if (request_reconcile) {
                service_request_topology_refresh_from_worker();
            }
        }

        service_finish_sensor_bit(bit);

        if (service_topology_refresh_pending() && mask != 0U) {
            service_requeue_inflight_mask(mask);
            return;
        }
    }

    if (mask != 0U) {
        service_requeue_inflight_mask(mask);
    }
}

static bool service_take_height_reference_request(uint8_t *mask, uint16_t *generation);
static void service_process_height_reference_mask(
    nrf_topology_platform_t *platform,
    nrf_sensor_transport_controller_t *sensor_controller,
    const nrf_topology_snapshot_t *snapshot,
    uint8_t mask,
    uint16_t generation);
static bool service_take_imu_calibration_request(uint8_t *mask, uint16_t *generation);
static void service_process_imu_calibration_mask(
    nrf_topology_platform_t *platform,
    nrf_sensor_transport_controller_t *sensor_controller,
    const nrf_topology_snapshot_t *snapshot,
    uint8_t mask,
    uint16_t generation);

static bool cs_node_present(const nrf_topology_snapshot_t *snapshot, nrf_cs_node_id_t node)
{
    if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED && snapshot->main.node_id == node) return true;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED && snapshot->remotes[channel].node_id == node) return true;
    }
    return false;
}

static size_t cs_collect_nodes(const nrf_topology_snapshot_t *snapshot, nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES], bool *overflow)
{
    size_t count = 0U;
    *overflow = false;
    if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) nodes[count++] = snapshot->main.node_id;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (snapshot->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        if (count >= NRF_CS_MAX_NODES) { *overflow = true; continue; }
        nodes[count++] = snapshot->remotes[channel].node_id;
    }
    return count;
}

static esp_err_t cs_route_open(nrf_cs_platform_context_t *ctx, nrf_cs_node_id_t node, i2c_bus_device_handle_t *device, bool *remote)
{
    esp_err_t err = platform_open_address_handles(ctx->platform);
    if (err != ESP_OK) return err;
    if (ctx->snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED && ctx->snapshot->main.node_id == node) {
        if (ctx->platform->mux_bound) (void)platform_mux_close(ctx->platform);
        *device = ctx->platform->main_device; *remote = false; return ESP_OK;
    }
    if (!ctx->snapshot->tca_available || ctx->snapshot->tca_fault || !ctx->platform->mux_bound) return ESP_ERR_NOT_FOUND;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (ctx->snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED && ctx->snapshot->remotes[channel].node_id == node) {
            err = platform_mux_select(ctx->platform, channel);
            if (err != ESP_OK) return err;
            *device = ctx->platform->remote_device; *remote = true; return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void cs_route_close(nrf_cs_platform_context_t *ctx, bool remote)
{
    if (remote && ctx->platform->mux_bound && platform_mux_close(ctx->platform) != ESP_OK) service_request_topology_refresh_from_worker();
}

static int cs_write_execute(void *context, const nrf_cs_control_request_t *request)
{
    nrf_cs_platform_context_t *ctx = context;
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    i2c_bus_device_handle_t device = NULL;
    bool remote = false;
    esp_err_t err;
    if (nrf_cs_control_encode_execute(request, frame) != 0) return -1;
    err = cs_route_open(ctx, request->step.initiator, &device, &remote);
    if (err == ESP_OK) {
        const uint8_t address = remote
            ? NRF_UPSTREAM_ADDRESS_REMOTE : NRF_UPSTREAM_ADDRESS_MAIN;
        err = platform_i2c_write_bytes(
            ctx->platform, device, address, sizeof(frame), frame);
        sensor_bus_record_transfer_result_with_context(
            address, err, s_i2c_diag_source,
            ctx->platform->selected_mux_channel_valid
                ? (int)ctx->platform->selected_mux_channel : -1);
    }
    cs_route_close(ctx, remote);
    if (err != ESP_OK) { service_request_topology_refresh_from_worker(); return -1; }
    return 0;
}

static int cs_poll_result(void *context, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *result)
{
    nrf_cs_platform_context_t *ctx = context;
    uint8_t selector[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    i2c_bus_device_handle_t device = NULL;
    bool remote = false;
    size_t route_index = 0U;
    uint8_t address = NRF_UPSTREAM_ADDRESS_MAIN;
    esp_err_t err;
    if (nrf_cs_control_encode_status_request(request, selector) != 0) return -1;
    err = cs_route_open(ctx, request->step.initiator, &device, &remote);
    if (err == ESP_OK) {
        if (remote) address = NRF_UPSTREAM_ADDRESS_REMOTE;
        err = platform_i2c_write_bytes(
            ctx->platform, device, address, sizeof(selector), selector);
        sensor_bus_record_transfer_result_with_context(
            address, err, s_i2c_diag_source,
            ctx->platform->selected_mux_channel_valid
                ? (int)ctx->platform->selected_mux_channel : -1);
    }
    if (err == ESP_OK) {
        err = platform_probe_before_read(ctx->platform, address, &route_index);
    }
    if (err == ESP_OK) {
        err = platform_i2c_read_bytes(
            ctx->platform, device, address, sizeof(frame), frame);
        platform_record_read_result(ctx->platform, address, route_index, err == ESP_OK);
        err = platform_normalize_transfer_error(ctx->platform, address, err);
    }
    cs_route_close(ctx, remote);
    if (err != ESP_OK) { service_request_topology_refresh_from_worker(); return -1; }
    return nrf_cs_control_decode_result(frame, sizeof(frame), result) == 0 ? 0 : -1;
}

static uint64_t cs_now_ms(void *context){return platform_now_ms(((nrf_cs_platform_context_t *)context)->platform);}

static void cs_publish(nrf_topology_worker_state_t *worker, bool active, int result)
{
    nrf_cs_cycle_snapshot_t *snapshot = &worker->cs_cycle_snapshot;
    const nrf_cs_authority_t *authority = &worker->authority;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = active;
    snapshot->complete = !active && nrf_cs_scheduler_is_complete(&authority->scheduler);
    snapshot->result = result;
    snapshot->cycle_id = authority->scheduler.cycle_id;
    snapshot->membership_generation = authority->scheduler.membership_generation;
    snapshot->executions_done = nrf_cs_scheduler_executions_done(&authority->scheduler);
    snapshot->executions_total = nrf_cs_scheduler_execution_count(&authority->scheduler);
    snapshot->pair_count = nrf_cs_authority_pair_count(authority);
    for (size_t i = 0U; i < snapshot->pair_count && i < NRF_CS_MAX_PAIRS; ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_authority_pair_at(authority, i);
        if (pair != NULL) snapshot->pairs[i] = *pair;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_cs_cycle_snapshot = *snapshot;
    s_cs_snapshot_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool cs_take_request(
    nrf_cs_policy_t *policy,
    bool *is_ranging,
    uint32_t *ranging_timeout_ms,
    uint64_t *ranging_started_at_ms)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    pending = s_cs_request_pending;
    if (pending) { *policy = s_cs_requested_policy; s_cs_request_pending = false; }
    if (pending) {
        *is_ranging = s_cs_request_is_ranging;
        *ranging_timeout_ms = s_cs_requested_ranging_timeout_ms;
        *ranging_started_at_ms = s_cs_requested_ranging_started_ms;
        s_cs_request_is_ranging = false;
        s_cs_requested_ranging_timeout_ms = 0U;
        s_cs_requested_ranging_started_ms = 0U;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static void cs_sync_disconnects(nrf_cs_authority_t *authority, const nrf_topology_snapshot_t *snapshot)
{
    for (size_t i = 0U; i < authority->scheduler.node_count; ++i) {
        const nrf_cs_node_id_t node = authority->scheduler.nodes[i];
        if (!cs_node_present(snapshot, node)) (void)nrf_cs_authority_disconnect(authority, node);
    }
}

static void service_ranging_publish_progress(nrf_topology_worker_state_t *worker)
{
    worker->ranging_progress.active = worker->ranging_active;
    worker->ranging_progress.updated_at_ms = platform_now_ms(&worker->platform);
    portENTER_CRITICAL(&s_state_lock);
    s_ranging_progress = worker->ranging_progress;
    s_ranging_progress_valid = true;
    s_ranging_active = worker->ranging_active;
    portEXIT_CRITICAL(&s_state_lock);
}

#if NRF_TOPOLOGY_HAS_RANGING_SOLVER
static int32_t ranging_saturate_i32(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static nrf_geometry_result_t service_ranging_solve(
    nrf_topology_worker_state_t *worker,
    nrf_topology_ranging_geometry_t *geometry,
    int32_t *residual_mm,
    uint16_t *main_height_reference_generation)
{
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t input_count = 0U;
    device_status_snapshot_t device_status = {0};
    bool main_reference_valid = false;
    bool has_valid_pair = false;
    uint16_t main_reference_generation = 0U;
    int32_t main_relative_z_mm = 0;
    bool use_barometric_z = false;
    nrf_geometry_solution_t solution = {0};

    memset(geometry, 0, sizeof(*geometry));
    *residual_mm = INT32_MAX;
    *main_height_reference_generation = 0U;
    if (worker->snapshot.main.state != NRF_TOPOLOGY_NODE_ASSIGNED) {
        return NRF_GEOMETRY_ERR_NO_MAIN;
    }

    inputs[input_count++] = (nrf_geometry_node_input_t) {
        .node_id = worker->snapshot.main.node_id,
        .z_mm = 0,
    };
    if (device_status_get_snapshot(&device_status) == ESP_OK &&
        device_status.main.height_reference.state == DEVICE_MAIN_HEIGHT_REFERENCE_VALID &&
        device_status.main.height_reference.generation != 0U) {
        main_reference_valid = true;
        main_reference_generation = device_status.main.height_reference.generation;
        main_relative_z_mm = device_status.main.height_reference.relative_z_mm;
        use_barometric_z = true;
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (worker->snapshot.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        const nrf_remote_sensor_cache_t *cache = &worker->cache[channel];
        if (!main_reference_valid || !cache->response_valid || !cache->topology_current ||
            cache->sensor.reference_state != NRF_UPSTREAM_REFERENCE_VALID ||
            cache->sensor.reference_generation != main_reference_generation) {
            use_barometric_z = false;
        }
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (worker->snapshot.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        if (input_count >= NRF_GEOMETRY_MAX_NODES) {
            return NRF_GEOMETRY_ERR_ARGUMENT;
        }
        const nrf_remote_sensor_cache_t *cache = &worker->cache[channel];
        const int32_t z_mm = use_barometric_z
            ? ranging_saturate_i32(
                  (int64_t)cache->sensor.relative_z_mm - main_relative_z_mm)
            : 0;
        inputs[input_count++] = (nrf_geometry_node_input_t) {
            .node_id = worker->snapshot.remotes[channel].node_id,
            .z_mm = z_mm,
        };
    }

    const nrf_geometry_result_t result = nrf_geometry_solver_solve_partial(
        inputs,
        input_count,
        worker->cs_cycle_snapshot.pairs,
        worker->cs_cycle_snapshot.pair_count,
        &solution);
    if (result != NRF_GEOMETRY_OK) {
        return result;
    }
    for (size_t i = 0U; i < worker->cs_cycle_snapshot.pair_count; ++i) {
        has_valid_pair = has_valid_pair ||
            worker->cs_cycle_snapshot.pairs[i].state == NRF_CS_PAIR_VALID;
    }

    geometry->node_count = solution.node_count;
    geometry->max_residual_mm = solution.max_residual_mm;
    geometry->inconsistent_pair_count = solution.inconsistent_pair_count;
    geometry->refined = solution.refined;
    geometry->refine_iterations = solution.refine_iterations;
    for (size_t i = 0U; i < solution.node_count; ++i) {
        geometry->nodes[i] = (nrf_topology_ranging_node_t) {
            .node_id = solution.nodes[i].node_id,
            .solved = solution.nodes[i].solved,
            .x_mm = solution.nodes[i].x_mm,
            .y_mm = solution.nodes[i].y_mm,
            .z_mm = solution.nodes[i].z_mm,
            .residual_mm = solution.nodes[i].residual_mm,
            .uncertainty_mm = solution.nodes[i].solved
                ? solution.nodes[i].uncertainty_mm
                : INT32_MAX,
        };
    }
    *residual_mm = has_valid_pair ? solution.max_residual_mm : INT32_MAX;
    *main_height_reference_generation = use_barometric_z
        ? main_reference_generation
        : 0U;
    return NRF_GEOMETRY_OK;
}
#else
static int service_ranging_solve(
    nrf_topology_worker_state_t *worker,
    nrf_topology_ranging_geometry_t *geometry,
    int32_t *residual_mm,
    uint16_t *main_height_reference_generation)
{
    (void)worker;
    memset(geometry, 0, sizeof(*geometry));
    *residual_mm = INT32_MAX;
    *main_height_reference_generation = 0U;
    return -1;
}
#endif

static const nrf_topology_ranging_node_t *service_ranging_find_node(
    const nrf_topology_ranging_geometry_t *geometry,
    nrf_cs_node_id_t node_id)
{
    for (size_t i = 0U; i < geometry->node_count; ++i) {
        if (geometry->nodes[i].node_id == node_id) {
            return &geometry->nodes[i];
        }
    }
    return NULL;
}

static bool service_ranging_geometry_solves_all_nodes(
    const nrf_topology_worker_state_t *worker)
{
    size_t assigned_count = worker->snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    if (worker->snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        const nrf_topology_ranging_node_t *main = service_ranging_find_node(
            &worker->ranging_progress.geometry,
            worker->snapshot.main.node_id);
        if (main == NULL || !main->solved) return false;
    }
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (worker->snapshot.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        ++assigned_count;
        const nrf_topology_ranging_node_t *remote = service_ranging_find_node(
            &worker->ranging_progress.geometry,
            worker->snapshot.remotes[channel].node_id);
        if (remote == NULL || !remote->solved) return false;
    }
    return worker->ranging_progress.geometry.node_count == assigned_count;
}

static nrf_cs_node_id_t service_ranging_first_unsolved_node(
    const nrf_topology_worker_state_t *worker)
{
    if (worker->snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        const nrf_topology_ranging_node_t *main = service_ranging_find_node(
            &worker->ranging_progress.geometry,
            worker->snapshot.main.node_id);
        if (main == NULL || !main->solved) return worker->snapshot.main.node_id;
    }
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (worker->snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            const nrf_topology_ranging_node_t *remote = service_ranging_find_node(
                &worker->ranging_progress.geometry,
                worker->snapshot.remotes[channel].node_id);
            if (remote == NULL || !remote->solved) {
                return worker->snapshot.remotes[channel].node_id;
            }
        }
    }
    return 0U;
}

static void service_ranging_copy_cycle(nrf_topology_worker_state_t *worker)
{
    nrf_topology_ranging_progress_t *progress = &worker->ranging_progress;
    const nrf_cs_cycle_snapshot_t *cycle = &worker->cs_cycle_snapshot;
    progress->pairs_done = cycle->executions_done;
    progress->pairs_total = cycle->executions_total;
    progress->pair_count = cycle->pair_count;
    progress->membership_generation = cycle->membership_generation;
    if (progress->pair_count > NRF_CS_MAX_PAIRS) {
        progress->pair_count = NRF_CS_MAX_PAIRS;
    }
    memcpy(progress->pairs, cycle->pairs, progress->pair_count * sizeof(progress->pairs[0]));
}

static bool ranging_member_precedes(
    const nrf_topology_ranging_member_identity_t *left,
    const nrf_topology_ranging_member_identity_t *right)
{
    if (left->channel != right->channel) return left->channel < right->channel;
    if (left->node_id != right->node_id) return left->node_id < right->node_id;
    const size_t common_length = left->hardware_id_length < right->hardware_id_length
        ? left->hardware_id_length
        : right->hardware_id_length;
    const int hardware_order = memcmp(left->hardware_id, right->hardware_id, common_length);
    if (hardware_order != 0) return hardware_order < 0;
    return left->hardware_id_length < right->hardware_id_length;
}

static bool service_ranging_capture_membership(
    const nrf_topology_snapshot_t *topology,
    nrf_topology_ranging_member_identity_t membership[NRF_CS_MAX_NODES],
    size_t *membership_count)
{
    if (topology == NULL || membership == NULL || membership_count == NULL ||
        topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED) {
        return false;
    }

    size_t count = 0U;
    const nrf_topology_node_snapshot_t *node = &topology->main;
    if (node->hardware_id_length == 0U ||
        node->hardware_id_length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) {
        return false;
    }
    membership[count++] = (nrf_topology_ranging_member_identity_t) {
        .channel = NRF_UPSTREAM_MUX_CHANNEL_NONE,
        .node_id = node->node_id,
        .hardware_id_length = node->hardware_id_length,
    };
    memcpy(membership[0].hardware_id, node->hardware_id, node->hardware_id_length);

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        node = &topology->remotes[channel];
        if (node->state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        if (count >= NRF_CS_MAX_NODES || node->hardware_id_length == 0U ||
            node->hardware_id_length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) {
            return false;
        }
        membership[count] = (nrf_topology_ranging_member_identity_t) {
            .channel = channel,
            .node_id = node->node_id,
            .hardware_id_length = node->hardware_id_length,
        };
        memcpy(membership[count].hardware_id, node->hardware_id, node->hardware_id_length);
        ++count;
    }

    for (size_t i = 1U; i < count; ++i) {
        const nrf_topology_ranging_member_identity_t value = membership[i];
        size_t j = i;
        while (j > 0U && ranging_member_precedes(&value, &membership[j - 1U])) {
            membership[j] = membership[j - 1U];
            --j;
        }
        membership[j] = value;
    }
    *membership_count = count;
    return true;
}

static bool service_ranging_membership_matches(
    const nrf_topology_ranging_result_t *result,
    const nrf_topology_snapshot_t *topology)
{
    nrf_topology_ranging_member_identity_t current[NRF_CS_MAX_NODES] = {0};
    size_t current_count = 0U;
    if (result == NULL || !result->membership_identity_valid ||
        result->membership_count > NRF_CS_MAX_NODES ||
        !service_ranging_capture_membership(topology, current, &current_count) ||
        current_count != result->membership_count) {
        return false;
    }
    return memcmp(
        current,
        result->membership,
        current_count * sizeof(current[0])) == 0;
}

static void service_ranging_finish(
    nrf_topology_worker_state_t *worker,
    bool success,
    const char *failure_reason,
    uint64_t now_ms)
{
    nrf_topology_ranging_progress_t *progress = &worker->ranging_progress;
    progress->active = false;
    progress->complete = true;
    progress->success = success;
    progress->updated_at_ms = now_ms;
    if (failure_reason != NULL) {
        snprintf(progress->failure_reason, sizeof(progress->failure_reason), "%s", failure_reason);
    } else {
        progress->failure_reason[0] = '\0';
    }
    worker->ranging_active = false;

    if (success) {
        nrf_topology_ranging_result_t result = {0};
        result.available = true;
        result.pair_count = progress->pair_count;
        memcpy(result.pairs, progress->pairs, result.pair_count * sizeof(result.pairs[0]));
        result.geometry = progress->geometry;
        result.topology_generation = progress->topology_generation;
        result.membership_generation = progress->membership_generation;
        result.main_height_reference_generation = progress->main_height_reference_generation;
        result.timestamp_ms = now_ms;
        result.membership_identity_valid = service_ranging_capture_membership(
            &worker->snapshot,
            result.membership,
            &result.membership_count);
        portENTER_CRITICAL(&s_state_lock);
        s_frozen_ranging_result = result;
        s_frozen_ranging_result_valid = true;
        portEXIT_CRITICAL(&s_state_lock);
    }
    service_ranging_publish_progress(worker);
}

static void service_ranging_process(nrf_topology_worker_state_t *worker)
{
    nrf_topology_ranging_progress_t *progress = &worker->ranging_progress;
    const uint64_t now_ms = platform_now_ms(&worker->platform);
    if (!worker->ranging_active) {
        return;
    }

    if (nrf_cs_authority_is_active(&worker->authority) && now_ms >= worker->ranging_deadline_ms) {
        (void)nrf_cs_authority_abort_cycle(&worker->authority, NRF_CS_FAILURE_TIMEOUT);
        cs_publish(worker, false, NRF_CS_AUTHORITY_COMPLETE);
        service_ranging_copy_cycle(worker);
        service_ranging_finish(worker, false, "timeout", now_ms);
        return;
    }

    service_ranging_copy_cycle(worker);
    progress->topology_generation = worker->ranging_topology_generation;
    for (size_t i = 0U; i < progress->pair_count; ++i) {
        const uint16_t bit = (uint16_t)(1U << i);
        const nrf_cs_pair_result_t *pair = &progress->pairs[i];
        if ((worker->ranging_processed_pairs_mask & bit) != 0U ||
            pair->state == NRF_CS_PAIR_PENDING || pair->state == NRF_CS_PAIR_UNKNOWN) {
            continue;
        }
        worker->ranging_processed_pairs_mask |= bit;
        (void)service_ranging_solve(
            worker,
            &progress->geometry,
            &progress->solution_residual_mm,
            &progress->main_height_reference_generation);
        if (pair->state != NRF_CS_PAIR_VALID && !worker->ranging_pair_failure_seen) {
            worker->ranging_pair_failure_seen = true;
            worker->ranging_failed_pair_a = pair->node_a;
            worker->ranging_failed_pair_b = pair->node_b;
        }
    }
    service_ranging_publish_progress(worker);

    if (!nrf_cs_scheduler_is_complete(&worker->authority.scheduler)) {
        return;
    }

    (void)service_ranging_solve(
        worker,
        &progress->geometry,
        &progress->solution_residual_mm,
        &progress->main_height_reference_generation);
    service_ranging_copy_cycle(worker);
    progress->topology_generation = worker->ranging_topology_generation;
    if (service_ranging_geometry_solves_all_nodes(worker)) {
        service_ranging_finish(worker, true, NULL, now_ms);
    } else if (worker->ranging_pair_failure_seen) {
        char reason[NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE];
        (void)snprintf(
            reason,
            sizeof(reason),
            "pair_failed:%u-%u",
            (unsigned)worker->ranging_failed_pair_a,
            (unsigned)worker->ranging_failed_pair_b);
        service_ranging_finish(worker, false, reason, now_ms);
    } else {
        char reason[NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE];
        (void)snprintf(
            reason,
            sizeof(reason),
            "geometry_unsolved:%u",
            (unsigned)service_ranging_first_unsolved_node(worker));
        service_ranging_finish(worker, false, reason, now_ms);
    }
}

static void service_ranging_fail_start(nrf_topology_worker_state_t *worker)
{
    worker->ranging_active = false;
    worker->ranging_progress.active = false;
    worker->ranging_progress.complete = true;
    worker->ranging_progress.success = false;
    snprintf(
        worker->ranging_progress.failure_reason,
        sizeof(worker->ranging_progress.failure_reason),
        "geometry_unsolved:%u",
        (unsigned)NRF_TOPOLOGY_MAIN_NODE_ID);
    service_ranging_publish_progress(worker);
}

static void service_update_link_readiness(
    nrf_topology_worker_state_t *worker,
    uint64_t now_ms)
{
    nrf_link_readiness_t *main_link = &worker->links[0];
    if (worker->snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        if (!nrf_link_readiness_is_ready(main_link)) {
            nrf_link_readiness_record_ready(main_link, now_ms);
        }
    } else if (nrf_link_readiness_is_ready(main_link)) {
        nrf_link_readiness_record_not_ready(main_link, now_ms);
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        nrf_link_readiness_t *link = &worker->links[channel + 1U];
        if (worker->snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            if (!nrf_link_readiness_is_ready(link)) {
                nrf_link_readiness_record_ready(link, now_ms);
            }
        } else if (nrf_link_readiness_is_ready(link)) {
            nrf_link_readiness_record_not_ready(link, now_ms);
        }
    }
}

static void service_worker(void *context)
{
    (void)context;
    nrf_topology_worker_state_t *worker = &s_worker_state;
    const nrf_topology_config_t topology_config = {
        .confirm_timeout_ms = NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS,
        .poll_interval_ms = NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS,
        .request_seed = 0U,
    };
    const nrf_sensor_transport_config_t sensor_config = {
        .timeout_ms = NRF_SENSOR_TRANSPORT_DEFAULT_TIMEOUT_MS,
        .poll_interval_ms = NRF_SENSOR_TRANSPORT_DEFAULT_POLL_MS,
        .request_seed = 0U,
    };
    const TickType_t reconcile_period_ticks = pdMS_TO_TICKS(NRF_TOPOLOGY_RECONCILE_PERIOD_MS);
    const TickType_t sensor_refresh_period_ticks = pdMS_TO_TICKS(NRF_SENSOR_REFRESH_PERIOD_MS);
    TickType_t last_reconcile_tick = xTaskGetTickCount();
    TickType_t last_sensor_refresh_tick = last_reconcile_tick;
#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
    TickType_t last_log_poll_tick = (TickType_t)(last_reconcile_tick - pdMS_TO_TICKS(NRF_LOG_COLLECTOR_PERIOD_MS));
#endif
    bool first_reconcile = true;
    bool controllers_ready = false;
    bool cs_initialized = false;
    bool stop_before_start = service_register_current_task();

    memset(worker, 0, sizeof(*worker));
    const uint64_t links_started_ms = platform_now_ms(NULL);
    for (uint8_t i = 0U; i < NRF_LOG_COLLECTOR_NODE_COUNT; ++i) {
        nrf_link_readiness_init(&worker->links[i], links_started_ms);
    }
    worker->cs_context.platform = &worker->platform;
    worker->cs_context.snapshot = &worker->snapshot;
    worker->cs_ops = (nrf_cs_authority_ops_t) {
        .context = &worker->cs_context,
        .write_execute = cs_write_execute,
        .poll_result = cs_poll_result,
        .now_ms = cs_now_ms,
    };

    if (!stop_before_start) {
        nrf_topology_config_t runtime_topology_config = topology_config;
        nrf_sensor_transport_config_t runtime_sensor_config = sensor_config;
        runtime_topology_config.request_seed = esp_random();
        runtime_sensor_config.request_seed = esp_random();
        if (runtime_topology_config.request_seed == 0U) {
            runtime_topology_config.request_seed = 1U;
        }
        if (runtime_sensor_config.request_seed == 0U) {
            runtime_sensor_config.request_seed = 1U;
        }
        controllers_ready =
            nrf_topology_controller_init(&worker->controller, &runtime_topology_config) == ESP_OK &&
            nrf_sensor_transport_controller_init(&worker->sensor_controller, &runtime_sensor_config) == ESP_OK;
    }

    while (!stop_before_start && controllers_ready && !service_stop_requested()) {
        const TickType_t now = xTaskGetTickCount();
        const bool background_deferred = service_background_deferred(worker, cs_initialized);
        const bool explicit_refresh = !background_deferred &&
            service_take_topology_refresh_request();
        if (first_reconcile || (!background_deferred &&
            (explicit_refresh || tick_period_due(now, last_reconcile_tick, reconcile_period_ticks)))) {
            const bool was_first_reconcile = first_reconcile;
            const bool reconciled = service_run_topology_reconcile(
                &worker->platform,
                &worker->controller,
                &worker->snapshot,
                worker->cache);
            last_reconcile_tick = xTaskGetTickCount();
            first_reconcile = false;
            if (reconciled) {
                service_update_link_readiness(worker, platform_now_ms(NULL));
            }
            const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
            if (was_first_reconcile) {
                service_record_stack_hwm(
                    NRF_TOPOLOGY_STACK_PHASE_FIRST_RECONCILE,
                    reconciled ? "first-reconcile" : "first-reconcile-error",
                    free_bytes);
            }
            service_record_stack_hwm(
                NRF_TOPOLOGY_STACK_PHASE_RECONCILE,
                reconciled ? "reconcile" : "reconcile-error",
                free_bytes);
            if (cs_initialized && nrf_cs_authority_is_active(&worker->authority)) {
                cs_sync_disconnects(&worker->authority, &worker->snapshot);
            }
#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
            if (reconciled) service_log_sync_targets(worker);
#endif
        }

        if (service_stop_requested()) {
            break;
        }

        if (!service_background_deferred(worker, cs_initialized) &&
            tick_period_due(
                xTaskGetTickCount(),
                last_sensor_refresh_tick,
                sensor_refresh_period_ticks)) {
            uint8_t assigned_mask = 0U;
            if (!worker->snapshot.bus_busy && !worker->snapshot.stale &&
                worker->snapshot.tca_available && !worker->snapshot.tca_fault) {
                for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
                    if (worker->snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
                        assigned_mask |= (uint8_t)(1U << channel);
                    }
                }
            }
            service_queue_sensor_mask(assigned_mask, false);
            last_sensor_refresh_tick = xTaskGetTickCount();
        }

        nrf_cs_policy_t requested_policy;
        bool ranging_request = false;
        uint32_t ranging_timeout_ms = 0U;
        uint64_t ranging_started_at_ms = 0U;
        if (cs_take_request(
                &requested_policy,
                &ranging_request,
                &ranging_timeout_ms,
                &ranging_started_at_ms)) {
            nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];
            bool overflow = false;
            const size_t count = cs_collect_nodes(&worker->snapshot, nodes, &overflow);
            int start_result = NRF_CS_AUTHORITY_ERR_STATE;
            if (!overflow && count >= 2U) {
                if (!cs_initialized) {
                    start_result = nrf_cs_authority_init(
                        &worker->authority,
                        &requested_policy,
                        &worker->cs_ops,
                        esp_random());
                    cs_initialized = start_result == NRF_CS_AUTHORITY_OK;
                } else if (!nrf_cs_authority_is_active(&worker->authority)) {
                    worker->authority.scheduler.policy = requested_policy;
                }
                if (cs_initialized) {
                    start_result = nrf_cs_authority_start_cycle(&worker->authority, nodes, count);
                }
            }
            if (!cs_initialized || start_result < 0) {
                memset(&worker->cs_cycle_snapshot, 0, sizeof(worker->cs_cycle_snapshot));
                portENTER_CRITICAL(&s_state_lock);
                memset(&s_cs_cycle_snapshot, 0, sizeof(s_cs_cycle_snapshot));
                s_cs_cycle_snapshot.result = start_result;
                s_cs_snapshot_valid = true;
                portEXIT_CRITICAL(&s_state_lock);
                if (ranging_request) {
                    portENTER_CRITICAL(&s_state_lock);
                    worker->ranging_progress = s_ranging_progress;
                    portEXIT_CRITICAL(&s_state_lock);
                    service_ranging_fail_start(worker);
                }
            } else {
                cs_publish(worker, true, start_result);
                if (ranging_request) {
                    worker->ranging_active = true;
                    worker->ranging_deadline_ms =
                        ranging_started_at_ms + (uint64_t)ranging_timeout_ms;
                    worker->ranging_topology_generation = worker->snapshot.generation;
                    worker->ranging_processed_pairs_mask = 0U;
                    worker->ranging_pair_failure_seen = false;
                    memset(&worker->ranging_progress, 0, sizeof(worker->ranging_progress));
                    worker->ranging_progress.active = true;
                    worker->ranging_progress.started_at_ms = ranging_started_at_ms;
                    worker->ranging_progress.topology_generation = worker->snapshot.generation;
                    service_ranging_copy_cycle(worker);
                    (void)service_ranging_solve(
                        worker,
                        &worker->ranging_progress.geometry,
                        &worker->ranging_progress.solution_residual_mm,
                        &worker->ranging_progress.main_height_reference_generation);
                    service_ranging_publish_progress(worker);
                    service_ranging_process(worker);
                }
            }
            service_record_current_stack_hwm(
                NRF_TOPOLOGY_STACK_PHASE_CS_CYCLE_START,
                start_result < 0 ? "cs-cycle-start-error" : "cs-cycle-start");
        }

        if (service_stop_requested()) {
            break;
        }

        if (!service_background_deferred(worker, cs_initialized)) {
            uint8_t height_reference_mask = 0U;
            uint16_t height_reference_generation = 0U;
            if (service_take_height_reference_request(
                    &height_reference_mask,
                    &height_reference_generation)) {
                service_process_height_reference_mask(
                    &worker->platform,
                    &worker->sensor_controller,
                    &worker->snapshot,
                    height_reference_mask,
                    height_reference_generation);
            }

            uint8_t imu_calibration_mask = 0U;
            uint16_t imu_calibration_generation = 0U;
            if (service_take_imu_calibration_request(
                    &imu_calibration_mask,
                    &imu_calibration_generation)) {
                service_process_imu_calibration_mask(
                    &worker->platform,
                    &worker->sensor_controller,
                    &worker->snapshot,
                    imu_calibration_mask,
                    imu_calibration_generation);
            }

            const uint8_t sensor_mask = service_take_sensor_mask();
            if (sensor_mask != 0U) {
                service_process_sensor_mask(
                    &worker->platform,
                    &worker->sensor_controller,
                    &worker->snapshot,
                    worker->cache,
                    sensor_mask,
                    last_reconcile_tick,
                    reconcile_period_ticks);
                service_record_current_stack_hwm(
                    NRF_TOPOLOGY_STACK_PHASE_SENSOR_REFRESH,
                    "sensor-refresh");
            }
        }

        if (service_stop_requested()) {
            break;
        }

        if (cs_initialized && nrf_cs_authority_is_active(&worker->authority)) {
            const nrf_cs_authority_result_t cs_result = nrf_cs_authority_pump(&worker->authority);
            cs_publish(worker, nrf_cs_authority_is_active(&worker->authority), cs_result);
            service_ranging_process(worker);
            service_record_current_stack_hwm(
                NRF_TOPOLOGY_STACK_PHASE_CS_PUMP_PUBLISH,
                "cs-pump-publish");
        }

#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
        if (!service_stop_requested() && !service_background_deferred(worker, cs_initialized) &&
            !service_has_immediate_work() &&
            !tick_period_due(xTaskGetTickCount(), last_reconcile_tick, reconcile_period_ticks) &&
            tick_period_due(
                xTaskGetTickCount(),
                last_log_poll_tick,
                pdMS_TO_TICKS(NRF_LOG_COLLECTOR_PERIOD_MS))) {
            /* Quiet nodes take about 5 x 7 ms per second; a busy node about 35 ms. */
            service_log_collect_iteration(worker, last_reconcile_tick, reconcile_period_ticks);
            last_log_poll_tick = xTaskGetTickCount();
        }
#endif

        TickType_t wait_ticks = 0U;
        if (!service_has_immediate_work()) {
            if (cs_initialized && nrf_cs_authority_is_active(&worker->authority)) {
                wait_ticks = pdMS_TO_TICKS(NRF_CS_PUMP_INTERVAL_MS);
            } else {
                wait_ticks = tick_wait_until_period(
                    xTaskGetTickCount(),
                    last_reconcile_tick,
                    reconcile_period_ticks);
#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
                const TickType_t log_wait_ticks = tick_wait_until_period(
                    xTaskGetTickCount(),
                    last_log_poll_tick,
                    pdMS_TO_TICKS(NRF_LOG_COLLECTOR_PERIOD_MS));
                if (log_wait_ticks < wait_ticks) wait_ticks = log_wait_ticks;
#endif
                const TickType_t sensor_wait_ticks = tick_wait_until_period(
                    xTaskGetTickCount(),
                    last_sensor_refresh_tick,
                    sensor_refresh_period_ticks);
                if (sensor_wait_ticks < wait_ticks) wait_ticks = sensor_wait_ticks;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }

    if (worker->ranging_active) {
        (void)nrf_cs_authority_abort_cycle(&worker->authority, NRF_CS_FAILURE_TIMEOUT);
        cs_publish(worker, false, NRF_CS_AUTHORITY_COMPLETE);
        service_ranging_copy_cycle(worker);
        service_ranging_finish(
            worker,
            false,
            "timeout",
            platform_now_ms(&worker->platform));
    }

    platform_close(&worker->platform);
    memset(worker, 0, sizeof(*worker));
    service_clear_current_task();
    vTaskDelete(NULL);
}

esp_err_t nrf_topology_service_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_stop_requested) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL || s_starting) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    s_starting = true;
    s_snapshot_valid = false;
    s_sensor_cache_valid = false;
    s_topology_refresh_requested = false;
    s_sensor_pending_mask = 0U;
    s_sensor_inflight_mask = 0U;
    s_height_reference_pending = false;
    s_height_reference_pending_mask = 0U;
    s_height_reference_pending_generation = 0U;
    s_height_reference_in_flight = false;
    s_height_reference_in_flight_generation = 0U;
    s_imu_calibration_pending = false;
    s_imu_calibration_pending_mask = 0U;
    s_imu_calibration_pending_generation = 0U;
    s_cs_request_pending = false;
    s_cs_request_is_ranging = false;
    s_cs_requested_ranging_timeout_ms = 0U;
    s_cs_requested_ranging_started_ms = 0U;
    s_cs_snapshot_valid = false;
    s_ranging_active = false;
    s_ranging_progress_valid = false;
    s_frozen_ranging_result_valid = false;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    memset(&s_main_status_cache, 0, sizeof(s_main_status_cache));
    memset(&s_cs_requested_policy, 0, sizeof(s_cs_requested_policy));
    memset(&s_cs_cycle_snapshot, 0, sizeof(s_cs_cycle_snapshot));
    memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
    memset(&s_frozen_ranging_result, 0, sizeof(s_frozen_ranging_result));
    memset(&s_stack_stats, 0, sizeof(s_stack_stats));
    memset(&s_worker_state, 0, sizeof(s_worker_state));
    portEXIT_CRITICAL(&s_state_lock);

#if !defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
    if (s_nrf_log_collector == NULL) {
        nrf_log_collector_t *collector = nrf_log_collector_create();
        if (collector == NULL) {
            portENTER_CRITICAL(&s_state_lock);
            s_starting = false;
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_state_lock);
        s_nrf_log_collector = collector;
        portEXIT_CRITICAL(&s_state_lock);
    }
    const esp_err_t log_reset_err = nrf_log_collector_reset(s_nrf_log_collector);
    if (log_reset_err != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_state_lock);
        return log_reset_err;
    }
#endif

    TaskHandle_t task = NULL;
    const BaseType_t result = xTaskCreatePinnedToCore(
        service_worker,
        "nrf_topology",
        APP_TASK_STACK_NRF_TOPOLOGY,
        NULL,
        APP_TASK_PRIORITY_NRF_TOPOLOGY,
        &task,
        APP_TASK_CORE_SERVICES);
    if (result != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Topology/CS authority service started on core=%d", APP_TASK_CORE_SERVICES);
    return ESP_OK;
}

void nrf_topology_service_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = true;
    const TaskHandle_t task = s_task;
    const bool starting = s_starting;
    portEXIT_CRITICAL(&s_state_lock);

    if (task == NULL && !starting) {
        portENTER_CRITICAL(&s_state_lock);
        s_stop_requested = false;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    if (task != NULL) {
        (void)xTaskNotifyGive(task);
    }

    while (true) {
        portENTER_CRITICAL(&s_state_lock);
        const bool stopped = s_task == NULL && !s_starting;
        portEXIT_CRITICAL(&s_state_lock);
        if (stopped) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = false;
    s_topology_refresh_requested = false;
    s_sensor_pending_mask = 0U;
    s_sensor_inflight_mask = 0U;
    s_height_reference_pending = false;
    s_height_reference_pending_mask = 0U;
    s_height_reference_pending_generation = 0U;
    s_height_reference_in_flight = false;
    s_height_reference_in_flight_generation = 0U;
    s_imu_calibration_pending = false;
    s_imu_calibration_pending_mask = 0U;
    s_imu_calibration_pending_generation = 0U;
    s_cs_request_pending = false;
    s_cs_request_is_ranging = false;
    s_cs_requested_ranging_timeout_ms = 0U;
    s_cs_requested_ranging_started_ms = 0U;
    s_cs_snapshot_valid = false;
    s_ranging_active = false;
    memset(&s_cs_requested_policy, 0, sizeof(s_cs_requested_policy));
    memset(&s_cs_cycle_snapshot, 0, sizeof(s_cs_cycle_snapshot));
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t nrf_topology_service_request_refresh(void)
{
    TaskHandle_t task;
    bool available;
    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    available = task != NULL || s_starting;
    if (available) {
        s_topology_refresh_requested = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL) {
        (void)xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_snapshot_valid;
    if (valid) {
        *snapshot = s_snapshot;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t nrf_topology_service_request_sensor_refresh(uint8_t channel)
{
    TaskHandle_t task;
    bool available;
    bool queued = false;
    const uint8_t bit = channel < NRF_TOPOLOGY_REMOTE_COUNT
        ? (uint8_t)(1U << channel)
        : 0U;

    if (channel >= NRF_TOPOLOGY_REMOTE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    available = task != NULL || s_starting;
    if (available && (s_sensor_pending_mask & bit) == 0U &&
        (s_sensor_inflight_mask & bit) == 0U) {
        s_sensor_pending_mask |= bit;
        queued = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL && queued) {
        (void)xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_all_sensor_refresh(void)
{
    TaskHandle_t task;
    bool available;
    bool snapshot_valid;
    uint8_t mask = 0U;

    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    available = task != NULL || s_starting;
    snapshot_valid = s_snapshot_valid;
    if (available && snapshot_valid) {
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (s_snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
                mask |= (uint8_t)(1U << channel);
            }
        }
        mask &= (uint8_t)~(s_sensor_pending_mask | s_sensor_inflight_mask);
        s_sensor_pending_mask |= mask;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!available || !snapshot_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL && mask != 0U) {
        (void)xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation)
{
    TaskHandle_t task;
    uint8_t mask = 0U;

    if (requested_channel_mask == NULL || requested_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *requested_channel_mask = 0U;
    *requested_generation = 0U;

    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    if (task == NULL || !s_snapshot_valid || s_height_reference_pending ||
        s_height_reference_in_flight) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (s_snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            mask |= (uint8_t)(1U << channel);
        }
    }
    if (mask == 0U) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NOT_FOUND;
    }

    /* A wrapped token is reused only after the previous request has drained. */
    ++s_height_reference_generation;
    if (s_height_reference_generation == 0U) {
        ++s_height_reference_generation;
    }
    s_height_reference_pending = true;
    s_height_reference_pending_mask = mask;
    s_height_reference_pending_generation = s_height_reference_generation;
    *requested_channel_mask = mask;
    *requested_generation = s_height_reference_generation;
    portEXIT_CRITICAL(&s_state_lock);

    (void)xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_imu_calibration(
    uint16_t generation,
    uint8_t channel_mask)
{
    TaskHandle_t task;
    const uint8_t valid_mask = (uint8_t)((1U << NRF_TOPOLOGY_REMOTE_COUNT) - 1U);

    if (channel_mask == 0U || (channel_mask & (uint8_t)~valid_mask) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    if (task == NULL || !s_snapshot_valid || s_imu_calibration_pending) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if ((channel_mask & (uint8_t)(1U << channel)) != 0U &&
            s_snapshot.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_NOT_FOUND;
        }
    }
    s_imu_calibration_pending = true;
    s_imu_calibration_pending_mask = channel_mask;
    s_imu_calibration_pending_generation = generation;
    portEXIT_CRITICAL(&s_state_lock);

    (void)xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_sensor_snapshot(
    uint8_t channel,
    nrf_remote_sensor_cache_t *snapshot)
{
    if (channel >= NRF_TOPOLOGY_REMOTE_COUNT || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_sensor_cache_valid;
    if (valid) {
        *snapshot = s_sensor_cache[channel];
        const uint8_t bit = (uint8_t)(1U << channel);
        if ((s_sensor_pending_mask & bit) != 0U || (s_sensor_inflight_mask & bit) != 0U) {
            snapshot->state = NRF_SENSOR_CACHE_PENDING;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool service_node_identity_equal(
    const nrf_topology_node_snapshot_t *left,
    const nrf_topology_node_snapshot_t *right)
{
    return left != NULL && right != NULL &&
        left->hardware_id_length == right->hardware_id_length &&
        left->hardware_id_length > 0U &&
        left->hardware_id_length <= NRF_UPSTREAM_PROTOCOL_HW_ID_MAX &&
        memcmp(left->hardware_id, right->hardware_id, left->hardware_id_length) == 0 &&
        left->node_id == right->node_id &&
        left->mux_channel == right->mux_channel &&
        left->current_address == right->current_address &&
        left->assignment_request_id == right->assignment_request_id;
}

esp_err_t nrf_topology_service_get_node_status_snapshot(
    uint8_t node_slot,
    nrf_node_link_status_input_t *snapshot)
{
    if (node_slot > NRF_TOPOLOGY_REMOTE_COUNT || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (!s_snapshot_valid) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const nrf_topology_node_snapshot_t *node = node_slot == 0U
        ? &s_snapshot.main
        : &s_snapshot.remotes[node_slot - 1U];
    const nrf_upstream_status_cache_t *status_cache = node_slot == 0U
        ? &s_main_status_cache
        : &s_sensor_cache[node_slot - 1U].upstream_status;
    if (node_slot == 0U && node->state == NRF_TOPOLOGY_NODE_UNKNOWN &&
        (s_snapshot.bus_busy || s_snapshot.stale)) {
        node = &status_cache->identity;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->status_valid = status_cache->valid;
    snapshot->status_identity_matches = status_cache->identity_matches &&
        service_node_identity_equal(node, &status_cache->identity);
    snapshot->lifecycle = status_cache->lifecycle;
    snapshot->result = status_cache->result;
    snapshot->flags = status_cache->flags;
    snapshot->status_receipt_uptime_ms = status_cache->esp_receipt_uptime_ms;
    snapshot->assigned = node->state == NRF_TOPOLOGY_NODE_ASSIGNED ||
        (node->state == NRF_TOPOLOGY_NODE_CONFLICT &&
         snapshot->status_valid && snapshot->status_identity_matches &&
         snapshot->lifecycle == NRF_UPSTREAM_LIFECYCLE_BOOTING &&
         snapshot->result == NRF_UPSTREAM_RESULT_OK &&
         (snapshot->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U);
    if (node_slot != 0U && s_sensor_cache_valid) {
        const nrf_remote_sensor_cache_t *sensor_cache = &s_sensor_cache[node_slot - 1U];
        snapshot->sensor_result_valid = sensor_cache->response_valid;
        snapshot->sensor_result = sensor_cache->sensor.result;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool topology_valid = s_snapshot_valid;
    const bool sensor_valid = s_sensor_cache_valid;
    const esp_err_t result = nrf_topology_leveling_summary_compute(
        topology_valid,
        &s_snapshot,
        sensor_valid,
        s_sensor_cache,
        summary);
    portEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t nrf_topology_service_get_leveling_nodes(
    nrf_topology_leveling_nodes_t *nodes)
{
    portENTER_CRITICAL(&s_state_lock);
    const esp_err_t result = nrf_topology_leveling_nodes_compute(
        s_snapshot_valid,
        &s_snapshot,
        s_sensor_cache_valid,
        s_sensor_cache,
        nodes);
    portEXIT_CRITICAL(&s_state_lock);
    return result;
}

#if defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
esp_err_t nrf_topology_service_test_read_status_cached_probe(
    uint8_t address,
    nrf_upstream_status_t *status,
    bool after_log)
{
    nrf_topology_platform_t platform = {0};
    size_t route_index;
    size_t address_index;
    esp_err_t err;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = platform_open_address_handles(&platform);
    if (err != ESP_OK) {
        platform_close(&platform);
        return err;
    }
    route_index = platform_read_route_index(&platform);
    address_index = platform_read_address_index(address);
    if (route_index >= NRF_PLATFORM_READ_ROUTE_COUNT ||
        address_index >= NRF_PLATFORM_READ_ADDRESS_COUNT) {
        platform_close(&platform);
        return ESP_ERR_INVALID_ARG;
    }

    /* Model a prior successful address probe, as after a cached log exchange. */
    platform.read_probe_valid[route_index][address_index] = true;
    if (after_log) {
        uint8_t request[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH] = {
            'G', 'S', NRF_LOG_PROTOCOL_VERSION, NRF_LOG_MESSAGE_STATUS_REQUEST,
            NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH,
        };
        const size_t crc_index = sizeof(request) - NRF_LOG_PROTOCOL_CRC_LENGTH;
        const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(request, crc_index);
        request[crc_index] = (uint8_t)crc;
        request[crc_index + 1U] = (uint8_t)(crc >> 8U);
        err = platform_i2c_write_bytes(
            &platform, platform_device_for_address(&platform, address),
            address, sizeof(request), request);
        if (err != ESP_OK) {
            platform_close(&platform);
            return err;
        }
    }
    err = platform_read_status(&platform, address, status);
    platform_close(&platform);
    return err;
}

esp_err_t nrf_topology_service_test_direct_nrf_io(
    uint8_t address,
    bool write)
{
    nrf_topology_platform_t platform = {0};
    uint8_t rx[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH];
    static const uint8_t status_request[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH] = {
        0x47U, 0x53U, NRF_LOG_PROTOCOL_VERSION, NRF_LOG_MESSAGE_STATUS_REQUEST,
    };
    esp_err_t err;

    if (!platform_address_is_nrf(address)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_snapshot_valid) {
        platform.last_good_snapshot = s_snapshot;
        platform.last_good_snapshot_valid = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (platform_conflict_count(&platform) == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    err = platform_open_address_handles(&platform);
    if (err != ESP_OK) {
        platform_close(&platform);
        (void)sensor_bus_deinit();
        return err;
    }
    if (write) {
        err = platform_i2c_write_bytes(
            &platform,
            platform_device_for_address(&platform, address),
            address,
            sizeof(status_request),
            status_request);
    } else {
        err = platform_i2c_read_bytes(
            &platform,
            platform_device_for_address(&platform, address),
            address,
            sizeof(rx),
            rx);
    }
    platform_close(&platform);
    const esp_err_t cleanup_err = sensor_bus_deinit();
    if (err == ESP_OK) {
        err = cleanup_err;
    }
    return err;
}

void nrf_topology_service_test_set_height_reference_generation(uint16_t generation)
{
    portENTER_CRITICAL(&s_state_lock);
    s_height_reference_generation = generation;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t nrf_topology_service_test_publish_state(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT])
{
    if ((topology_valid && topology == NULL) ||
        (sensor_cache_valid && cache == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_snapshot_valid = topology_valid;
    s_sensor_cache_valid = sensor_cache_valid;
    if (topology_valid) {
        s_snapshot = *topology;
    } else {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
    }
    if (sensor_cache_valid) {
        memcpy(s_sensor_cache, cache, sizeof(s_sensor_cache));
    } else {
        memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    }
    memset(&s_main_status_cache, 0, sizeof(s_main_status_cache));
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}
#endif

esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy)
{
    TaskHandle_t task;
    bool available;
    if (policy == NULL || policy->pair_timeout_ms == 0U || policy->max_attempts == 0U ||
        policy->min_confidence > NRF_CS_CONFIDENCE_MAX ||
        policy->samples_per_pair > NRF_CS_MAX_SAMPLES_PER_EXECUTION) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    available = task != NULL || s_starting;
    if (available && !s_ranging_active && !s_cs_request_pending &&
        (!s_cs_snapshot_valid || !s_cs_cycle_snapshot.active)) {
        s_cs_requested_policy = *policy;
        s_cs_request_pending = true;
        s_cs_request_is_ranging = false;
    } else if (available) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL) {
        (void)xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot)
{
    bool valid;
    if(snapshot==NULL)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock); valid=s_cs_snapshot_valid; if(valid)*snapshot=s_cs_cycle_snapshot; portEXIT_CRITICAL(&s_state_lock);
    return valid?ESP_OK:ESP_ERR_INVALID_STATE;
}

esp_err_t nrf_topology_service_ranging_start(
    const nrf_cs_policy_t *policy,
    uint32_t timeout_ms)
{
    TaskHandle_t task;
    nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];
    size_t node_count = 0U;
    const int64_t now_us = esp_timer_get_time();

    if (policy == NULL || !policy->full_rotation || policy->pair_timeout_ms == 0U ||
        policy->max_attempts == 0U || policy->min_confidence > NRF_CS_CONFIDENCE_MAX ||
        policy->samples_per_pair > NRF_CS_MAX_SAMPLES_PER_EXECUTION) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0U) {
        timeout_ms = NRF_TOPOLOGY_RANGING_DEFAULT_TIMEOUT_MS;
    }

    portENTER_CRITICAL(&s_state_lock);
    task = s_task;
    if (task == NULL || !s_snapshot_valid) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_snapshot.main.state != NRF_TOPOLOGY_NODE_ASSIGNED) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ++node_count;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        node_count += s_snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    }
    if (node_count < 2U) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (node_count > NRF_CS_MAX_NODES) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_ranging_active || s_cs_request_pending ||
        (s_cs_snapshot_valid && s_cs_cycle_snapshot.active)) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    node_count = 0U;
    nodes[node_count++] = s_snapshot.main.node_id;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (s_snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            nodes[node_count++] = s_snapshot.remotes[channel].node_id;
        }
    }
    for (size_t i = 1U; i < node_count; ++i) {
        const nrf_cs_node_id_t value = nodes[i];
        size_t j = i;
        while (j > 0U && nodes[j - 1U] > value) {
            nodes[j] = nodes[j - 1U];
            --j;
        }
        nodes[j] = value;
    }

    s_cs_requested_policy = *policy;
    s_cs_request_pending = true;
    s_cs_request_is_ranging = true;
    s_cs_requested_ranging_timeout_ms = timeout_ms;
    s_cs_requested_ranging_started_ms = now_us > 0 ? (uint64_t)now_us / 1000U : 0U;
    s_ranging_active = true;
    s_ranging_progress_valid = true;
    memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
    s_ranging_progress.active = true;
    s_ranging_progress.topology_generation = s_snapshot.generation;
    s_ranging_progress.pairs_total = node_count * (node_count - 1U);
    s_ranging_progress.started_at_ms = s_cs_requested_ranging_started_ms;
    s_ranging_progress.updated_at_ms = s_cs_requested_ranging_started_ms;
    s_ranging_progress.solution_residual_mm = INT32_MAX;
    s_ranging_progress.pair_count = node_count * (node_count - 1U) / 2U;
    s_ranging_progress.geometry.node_count = node_count;
    for (size_t i = 0U; i < node_count; ++i) {
        const bool is_main = nodes[i] == s_snapshot.main.node_id;
        s_ranging_progress.geometry.nodes[i].node_id = nodes[i];
        s_ranging_progress.geometry.nodes[i].solved = is_main;
        s_ranging_progress.geometry.nodes[i].uncertainty_mm = is_main ? 0 : INT32_MAX;
    }
    size_t pair_index = 0U;
    for (size_t left = 0U; left < node_count; ++left) {
        for (size_t right = left + 1U; right < node_count; ++right) {
            s_ranging_progress.pairs[pair_index].node_a = nodes[left];
            s_ranging_progress.pairs[pair_index].node_b = nodes[right];
            s_ranging_progress.pairs[pair_index].state = NRF_CS_PAIR_PENDING;
            ++pair_index;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    (void)xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress)
{
    if (progress == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_ranging_progress_valid;
    if (valid) {
        *progress = s_ranging_progress;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t nrf_topology_service_get_frozen_ranging_result(
    nrf_topology_ranging_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_frozen_ranging_result_valid;
    if (valid) {
        *result = s_frozen_ranging_result;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t nrf_topology_service_get_reusable_frozen_ranging_result(
    const nrf_topology_snapshot_t *current_topology,
    uint64_t now_ms,
    uint64_t max_age_ms,
    nrf_topology_ranging_result_t *result)
{
    if (current_topology == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_age_ms == 0U) {
        max_age_ms = NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS;
    }

    portENTER_CRITICAL(&s_state_lock);
    const bool valid = s_frozen_ranging_result_valid &&
        !s_frozen_ranging_result.invalidated &&
        now_ms >= s_frozen_ranging_result.timestamp_ms &&
        now_ms - s_frozen_ranging_result.timestamp_ms < max_age_ms &&
        service_ranging_membership_matches(&s_frozen_ranging_result, current_topology);
    if (valid) {
        *result = s_frozen_ranging_result;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t nrf_topology_service_invalidate_ranging(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_frozen_ranging_result_valid) {
        s_frozen_ranging_result.invalidated = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static bool service_take_height_reference_request(uint8_t *mask, uint16_t *generation)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    pending = s_height_reference_pending;
    if (pending) {
        *mask = s_height_reference_pending_mask;
        *generation = s_height_reference_pending_generation;
        s_height_reference_pending = false;
        s_height_reference_pending_mask = 0U;
        s_height_reference_pending_generation = 0U;
        s_height_reference_in_flight = true;
        s_height_reference_in_flight_generation = *generation;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static bool service_take_imu_calibration_request(uint8_t *mask, uint16_t *generation)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    pending = s_imu_calibration_pending;
    if (pending) {
        *mask = s_imu_calibration_pending_mask;
        *generation = s_imu_calibration_pending_generation;
        s_imu_calibration_pending = false;
        s_imu_calibration_pending_mask = 0U;
        s_imu_calibration_pending_generation = 0U;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static void service_process_height_reference_mask(
    nrf_topology_platform_t *platform,
    nrf_sensor_transport_controller_t *sensor_controller,
    const nrf_topology_snapshot_t *snapshot,
    uint8_t mask,
    uint16_t generation)
{
    const nrf_sensor_transport_io_t io = {
        .context = platform,
        .mux_select = platform_mux_select,
        .mux_close = platform_mux_close,
        .mux_faulted = platform_mux_faulted,
        .read_status = platform_read_status,
        .write_sensor_refresh = platform_write_sensor_refresh,
        .write_height_reference = platform_write_height_reference,
        .read_remote_snapshot = platform_read_remote_snapshot,
        .sleep_ms = platform_sleep_ms,
        .now_ms = platform_now_ms,
        .abort_requested = platform_abort_requested,
    };

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if ((mask & (uint8_t)(1U << channel)) == 0U) {
            continue;
        }
        bool request_reconcile = false;
        const esp_err_t err = nrf_sensor_transport_capture_height_reference(
            sensor_controller,
            &io,
            snapshot,
            channel,
            generation,
            &request_reconcile);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Height reference CAPTURE failed channel=%u generation=%u: %s",
                     (unsigned)channel, (unsigned)generation, esp_err_to_name(err));
        }
        if (request_reconcile) {
            service_request_topology_refresh_from_worker();
        }
    }

    portENTER_CRITICAL(&s_state_lock);
    if (s_height_reference_in_flight &&
        s_height_reference_in_flight_generation == generation) {
        s_height_reference_in_flight = false;
        s_height_reference_in_flight_generation = 0U;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_process_imu_calibration_mask(
    nrf_topology_platform_t *platform,
    nrf_sensor_transport_controller_t *sensor_controller,
    const nrf_topology_snapshot_t *snapshot,
    uint8_t mask,
    uint16_t generation)
{
    const nrf_sensor_transport_io_t io = {
        .context = platform,
        .mux_select = platform_mux_select,
        .mux_close = platform_mux_close,
        .mux_faulted = platform_mux_faulted,
        .read_status = platform_read_status,
        .write_imu_calibration = platform_write_imu_calibration,
        .sleep_ms = platform_sleep_ms,
        .now_ms = platform_now_ms,
        .abort_requested = platform_abort_requested,
    };

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if ((mask & (uint8_t)(1U << channel)) == 0U) {
            continue;
        }
        bool request_reconcile = false;
        const esp_err_t err = nrf_sensor_transport_request_imu_calibration(
            sensor_controller,
            &io,
            snapshot,
            channel,
            generation,
            &request_reconcile);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IMU calibration CAPTURE failed channel=%u generation=%u: %s",
                     (unsigned)channel, (unsigned)generation, esp_err_to_name(err));
        }
        if (request_reconcile) {
            service_request_topology_refresh_from_worker();
        }
    }
}

esp_err_t nrf_topology_service_get_stack_stats(nrf_topology_service_stack_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    *stats = s_stack_stats;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_nrf_log_summary(
    uint8_t node_index,
    nrf_log_node_summary_t *summary)
{
#if defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
    (void)node_index;
    (void)summary;
    return ESP_ERR_INVALID_STATE;
#else
    nrf_log_collector_t *collector;
    portENTER_CRITICAL(&s_state_lock);
    collector = s_nrf_log_collector;
    portEXIT_CRITICAL(&s_state_lock);
    return collector == NULL
        ? ESP_ERR_INVALID_STATE
        : nrf_log_collector_get_summary(collector, node_index, summary);
#endif
}

esp_err_t nrf_topology_service_get_nrf_log_page(
    uint8_t node_index,
    bool has_after,
    uint32_t after_boot_id,
    uint32_t after_seq,
    uint16_t limit,
    nrf_log_node_page_t *page)
{
#if defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
    (void)node_index;
    (void)has_after;
    (void)after_boot_id;
    (void)after_seq;
    (void)limit;
    (void)page;
    return ESP_ERR_INVALID_STATE;
#else
    nrf_log_collector_t *collector;
    portENTER_CRITICAL(&s_state_lock);
    collector = s_nrf_log_collector;
    portEXIT_CRITICAL(&s_state_lock);
    return collector == NULL
        ? ESP_ERR_INVALID_STATE
        : nrf_log_collector_get_page(
              collector,
              node_index,
              has_after,
              after_boot_id,
              after_seq,
              limit,
              page);
#endif
}
