#include "nrf_topology_service.h"
#include "nrf_sensor_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "sensor_bus.h"
#include "tca9548a_mux.h"
#include "tca9548a_mux_i2c_bus.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NRF_TOPOLOGY_RECONCILE_PERIOD_MS 5000U

static const char *TAG = "NRF_TOPOLOGY";

typedef struct {
    sensor_bus_handles_t bus_handles;
    i2c_bus_device_handle_t discovery_device;
    i2c_bus_device_handle_t main_device;
    i2c_bus_device_handle_t remote_device;
    i2c_bus_device_handle_t tca_device;
    uint8_t tca_address;
    tca9548a_mux_t mux;
    bool mux_bound;
} nrf_topology_platform_t;

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_starting = false;
static bool s_stop_requested = false;
static bool s_topology_refresh_requested = false;
static bool s_snapshot_valid = false;
static bool s_sensor_cache_valid = false;
static uint8_t s_sensor_pending_mask = 0U;
static uint8_t s_sensor_inflight_mask = 0U;
static nrf_topology_snapshot_t s_snapshot;
static nrf_remote_sensor_cache_t s_sensor_cache[NRF_TOPOLOGY_REMOTE_COUNT];

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
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_publish_state(
    const nrf_topology_snapshot_t *snapshot,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT])
{
    portENTER_CRITICAL(&s_state_lock);
    s_snapshot = *snapshot;
    memcpy(s_sensor_cache, cache, sizeof(s_sensor_cache));
    s_snapshot_valid = true;
    s_sensor_cache_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_publish_sensor_channel(
    uint8_t channel,
    const nrf_remote_sensor_cache_t *cache)
{
    portENTER_CRITICAL(&s_state_lock);
    s_sensor_cache[channel] = *cache;
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
    pending = s_topology_refresh_requested || s_sensor_pending_mask != 0U;
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
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

static esp_err_t platform_normalize_transfer_error(esp_err_t err)
{
    /* i2c_bus/legacy ESP-IDF reports a slave NACK as ESP_FAIL. */
    return err == ESP_FAIL ? ESP_ERR_NOT_FOUND : err;
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

    err = i2c_bus_read_bytes(
        device,
        NULL_I2C_MEM_ADDR,
        sizeof(frame),
        frame);
    if (err != ESP_OK) {
        return platform_normalize_transfer_error(err);
    }

    return nrf_upstream_protocol_decode_status(frame, sizeof(frame), status) == NRF_UPSTREAM_RESULT_OK
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
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

    err = i2c_bus_write_bytes(
        device,
        NULL_I2C_MEM_ADDR,
        sizeof(frame),
        frame);
    return err == ESP_OK ? ESP_OK : platform_normalize_transfer_error(err);
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
    err = i2c_bus_write_bytes(
        platform->remote_device,
        NULL_I2C_MEM_ADDR,
        sizeof(frame),
        frame);
    return err == ESP_OK ? ESP_OK : platform_normalize_transfer_error(err);
}

static esp_err_t platform_read_remote_snapshot(
    void *context,
    nrf_upstream_status_t *status,
    nrf_upstream_sensor_snapshot_t *sensor)
{
    nrf_topology_platform_t *platform = context;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    esp_err_t err;

    if (platform == NULL || status == NULL || sensor == NULL || platform->remote_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = i2c_bus_read_bytes(
        platform->remote_device,
        NULL_I2C_MEM_ADDR,
        sizeof(frame),
        frame);
    if (err != ESP_OK) {
        return platform_normalize_transfer_error(err);
    }
    return nrf_upstream_protocol_decode_remote_snapshot(frame, sizeof(frame), status, sensor) == NRF_UPSTREAM_RESULT_OK
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t platform_mux_close(void *context)
{
    nrf_topology_platform_t *platform = context;
    if (platform == NULL || !platform->mux_bound) {
        return ESP_ERR_INVALID_STATE;
    }
    return tca9548a_mux_close(&platform->mux);
}

static esp_err_t platform_mux_select(void *context, uint8_t channel)
{
    nrf_topology_platform_t *platform = context;
    if (platform == NULL || !platform->mux_bound) {
        return ESP_ERR_INVALID_STATE;
    }
    return tca9548a_mux_select(&platform->mux, channel);
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
        (void)i2c_bus_device_delete(device);
    }
}

static void platform_close(nrf_topology_platform_t *platform)
{
    if (platform == NULL) {
        return;
    }
    if (platform->mux_bound) {
        const esp_err_t close_err = tca9548a_mux_close(&platform->mux);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "TCA9548A final close failed: %s", esp_err_to_name(close_err));
        }
    }
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
        platform->discovery_device = i2c_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_DISCOVERY,
            SENSOR_BUS_I2C_SPEED_HZ);
        if (platform->discovery_device == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (platform->main_device == NULL) {
        platform->main_device = i2c_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_MAIN,
            SENSOR_BUS_I2C_SPEED_HZ);
        if (platform->main_device == NULL) {
            platform_delete_device(&platform->discovery_device);
            return ESP_ERR_NO_MEM;
        }
    }
    if (platform->remote_device == NULL) {
        platform->remote_device = i2c_bus_device_create(
            platform->bus_handles.bus,
            NRF_UPSTREAM_ADDRESS_REMOTE,
            SENSOR_BUS_I2C_SPEED_HZ);
        if (platform->remote_device == NULL) {
            platform_delete_device(&platform->main_device);
            platform_delete_device(&platform->discovery_device);
            return ESP_ERR_NO_MEM;
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
        return;
    }

    err = tca9548a_mux_bind_i2c_bus(&platform->mux, tca);
    if (err != ESP_OK) {
        *fault = true;
        return;
    }
    platform->tca_device = tca;
    platform->tca_address = address;
    platform->mux_bound = true;
    *available = true;
    ESP_LOGI(TAG, "TCA9548A topology mux available at 0x%02X", (unsigned)address);
}

static void service_log_snapshot(const nrf_topology_snapshot_t *snapshot)
{
    unsigned assigned = snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    unsigned conflicts = snapshot->main.state == NRF_TOPOLOGY_NODE_CONFLICT ? 1U : 0U;
    for (size_t index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        assigned += snapshot->remotes[index].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
        conflicts += snapshot->remotes[index].state == NRF_TOPOLOGY_NODE_CONFLICT ? 1U : 0U;
    }
    ESP_LOGI(
        TAG,
        "generation=%u tca=%s%s assigned=%u conflicts=%u main_state=%d",
        (unsigned)snapshot->generation,
        snapshot->tca_available ? "yes" : "no",
        snapshot->tca_fault ? "/fault" : "",
        assigned,
        conflicts,
        (int)snapshot->main.state);
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
    const esp_err_t open_err = platform_open_address_handles(platform);
    esp_err_t reconcile_err;

    if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "nRF topology address handles unavailable: %s", esp_err_to_name(open_err));
        const nrf_topology_io_t failed_io = {
            .context = platform,
            .tca_available = false,
            .tca_fault = true,
            .mux_close = platform_mux_close,
            .mux_select = platform_mux_select,
            .read_status = platform_read_status,
            .write_assign = platform_write_assign,
            .sleep_ms = platform_sleep_ms,
            .abort_requested = platform_abort_requested,
        };
        reconcile_err = nrf_topology_reconcile(controller, &failed_io, snapshot);
    }
    else {
        bool tca_available;
        bool tca_fault;
        platform_prepare_tca(platform, &tca_available, &tca_fault);
        const nrf_topology_io_t io = {
            .context = platform,
            .tca_available = tca_available,
            .tca_fault = tca_fault,
            .mux_close = platform_mux_close,
            .mux_select = platform_mux_select,
            .read_status = platform_read_status,
            .write_assign = platform_write_assign,
            .sleep_ms = platform_sleep_ms,
            .abort_requested = platform_abort_requested,
        };
        reconcile_err = nrf_topology_reconcile(controller, &io, snapshot);
    }

    if (reconcile_err != ESP_OK) {
        return false;
    }

    const uint8_t initial_mask = nrf_sensor_cache_reconcile_topology(snapshot, cache);
    service_publish_state(snapshot, cache);
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
        .read_remote_snapshot = platform_read_remote_snapshot,
        .sleep_ms = platform_sleep_ms,
        .now_ms = platform_now_ms,
        .abort_requested = platform_abort_requested,
    };

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
            service_publish_sensor_channel(channel, &cache[channel]);
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

static void service_worker(void *context)
{
    (void)context;
    nrf_topology_platform_t platform;
    nrf_topology_controller_t controller;
    nrf_sensor_transport_controller_t sensor_controller;
    nrf_topology_snapshot_t snapshot;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
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
    TickType_t last_reconcile_tick = xTaskGetTickCount();
    bool first_reconcile = true;
    bool controllers_ready = false;
    bool stop_before_start = service_register_current_task();

    memset(&platform, 0, sizeof(platform));
    memset(&snapshot, 0, sizeof(snapshot));
    memset(cache, 0, sizeof(cache));

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
            nrf_topology_controller_init(&controller, &runtime_topology_config) == ESP_OK &&
            nrf_sensor_transport_controller_init(&sensor_controller, &runtime_sensor_config) == ESP_OK;
    }

    while (!stop_before_start && controllers_ready && !service_stop_requested()) {
        const TickType_t now = xTaskGetTickCount();
        const bool explicit_refresh = service_take_topology_refresh_request();
        if (first_reconcile || explicit_refresh ||
            tick_period_due(now, last_reconcile_tick, reconcile_period_ticks)) {
            (void)service_run_topology_reconcile(
                &platform,
                &controller,
                &snapshot,
                cache);
            last_reconcile_tick = xTaskGetTickCount();
            first_reconcile = false;
        }

        if (service_stop_requested()) {
            break;
        }

        const uint8_t sensor_mask = service_take_sensor_mask();
        if (sensor_mask != 0U) {
            service_process_sensor_mask(
                &platform,
                &sensor_controller,
                &snapshot,
                cache,
                sensor_mask,
                last_reconcile_tick,
                reconcile_period_ticks);
        }

        if (service_stop_requested()) {
            break;
        }

        TickType_t wait_ticks = 0U;
        if (!service_has_immediate_work()) {
            const TickType_t wait_now = xTaskGetTickCount();
            wait_ticks = tick_wait_until_period(
                wait_now,
                last_reconcile_tick,
                reconcile_period_ticks);
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }

    platform_close(&platform);
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
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    portEXIT_CRITICAL(&s_state_lock);

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

    ESP_LOGI(TAG, "Topology service started on core=%d", APP_TASK_CORE_SERVICES);
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
