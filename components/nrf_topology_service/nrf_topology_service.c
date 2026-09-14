#include "nrf_topology_service.h"

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
static bool s_snapshot_valid = false;
static nrf_topology_snapshot_t s_snapshot;

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
    portEXIT_CRITICAL(&s_state_lock);
}

static void service_publish_snapshot(const nrf_topology_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_state_lock);
    s_snapshot = *snapshot;
    s_snapshot_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
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

static void platform_sleep_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
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

static void service_worker(void *context)
{
    (void)context;
    nrf_topology_platform_t platform;
    nrf_topology_controller_t controller;
    nrf_topology_snapshot_t snapshot;
    const nrf_topology_config_t config = {
        .confirm_timeout_ms = NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS,
        .poll_interval_ms = NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS,
        .request_seed = 0U,
    };
    bool controller_ready = false;
    bool stop_before_start = service_register_current_task();

    memset(&platform, 0, sizeof(platform));
    memset(&snapshot, 0, sizeof(snapshot));

    if (!stop_before_start) {
        nrf_topology_config_t runtime_config = config;
        runtime_config.request_seed = esp_random();
        if (runtime_config.request_seed == 0U) {
            runtime_config.request_seed = 1U;
        }
        controller_ready = nrf_topology_controller_init(&controller, &runtime_config) == ESP_OK;
    }

    while (!stop_before_start && controller_ready && !service_stop_requested()) {
        const esp_err_t open_err = platform_open_address_handles(&platform);
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "nRF topology address handles unavailable: %s", esp_err_to_name(open_err));
            const nrf_topology_io_t failed_io = {
                .context = &platform,
                .tca_available = false,
                .tca_fault = true,
                .mux_close = platform_mux_close,
                .mux_select = platform_mux_select,
                .read_status = platform_read_status,
                .write_assign = platform_write_assign,
                .sleep_ms = platform_sleep_ms,
                .abort_requested = platform_abort_requested,
            };
            if (nrf_topology_reconcile(&controller, &failed_io, &snapshot) == ESP_OK) {
                service_publish_snapshot(&snapshot);
            }
        }
        else {
            bool tca_available;
            bool tca_fault;
            platform_prepare_tca(&platform, &tca_available, &tca_fault);
            const nrf_topology_io_t io = {
                .context = &platform,
                .tca_available = tca_available,
                .tca_fault = tca_fault,
                .mux_close = platform_mux_close,
                .mux_select = platform_mux_select,
                .read_status = platform_read_status,
                .write_assign = platform_write_assign,
                .sleep_ms = platform_sleep_ms,
                .abort_requested = platform_abort_requested,
            };

            if (nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK) {
                service_publish_snapshot(&snapshot);
                service_log_snapshot(&snapshot);
            }
        }

        if (service_stop_requested()) {
            break;
        }
        (void)ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(NRF_TOPOLOGY_RECONCILE_PERIOD_MS));
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
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t nrf_topology_service_request_refresh(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const TaskHandle_t task = s_task;
    const bool starting = s_starting;
    portEXIT_CRITICAL(&s_state_lock);
    if (task != NULL) {
        (void)xTaskNotifyGive(task);
        return ESP_OK;
    }
    return starting ? ESP_OK : ESP_ERR_INVALID_STATE;
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
