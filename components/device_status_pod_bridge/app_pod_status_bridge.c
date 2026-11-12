#include "app_pod_status_bridge.h"
#include "app_pod_status_bridge_internal.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#if !defined(ESP_PLATFORM)
#include <stdlib.h>
#endif

#include "app_pod_status_barometer_policy.h"
#include "app_pod_status_inventory_policy.h"
#include "app_pod_status_local_position_policy.h"
#include "app_pod_status_state_policy.h"
#include "app_pod_status_sensor_health_policy.h"
#include "app_pod_status_sensor_values_policy.h"
#include "app_task_config.h"
#include "device_status.h"
#include "nrf_topology_service.h"
#include "object_store.h"
#include "runtime_observability.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"


#define APP_POD_STATUS_BRIDGE_PERIOD_MS 2000U
#define APP_POD_STATUS_BRIDGE_REFERENCE_PRESSURE_PA 101325.0f

/* The bridge indexes object_store's per-pod geometry array with the same
 * channel index device_status uses; this must hold or that indexing is
 * silently wrong. MAX-118 already confirmed both are 4 in practice -- this
 * just makes that invariant load-bearing and visible if it ever changes. */
_Static_assert(
    DEVICE_STATUS_POD_COUNT == OBJECT_STORE_POD_COUNT,
    "device_status and object_store must agree on pod count");


static const char *TAG =
    "POD_STATUS_BRIDGE";

static TaskHandle_t s_task =
    NULL;

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_starting =
    false;

static bool s_stop_requested =
    false;

typedef struct {
    object_store_summary_t active;
    object_store_profile_t profile;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t sensor_cache;
} app_pod_status_bridge_scratch_t;

/* Single consumer: production cycles run on bridge_task; host calls are synchronous. */
static app_pod_status_bridge_scratch_t *s_scratch =
    NULL;
static bool s_scratch_allocation_warning_logged =
    false;

/* Keep the non-profile publications available if the combined scratch cannot
 * be allocated. These are module storage, never task-stack temporaries. */
static nrf_topology_snapshot_t s_topology_fallback;
static nrf_remote_sensor_cache_t s_sensor_cache_fallback;

#if !defined(ESP_PLATFORM)
static unsigned s_test_scratch_allocations_to_fail;
static unsigned s_test_scratch_allocation_attempts;
static unsigned s_test_scratch_allocation_count;

static void *bridge_scratch_allocate(size_t size)
{
    ++s_test_scratch_allocation_attempts;
    if (s_test_scratch_allocations_to_fail > 0U) {
        --s_test_scratch_allocations_to_fail;
        return NULL;
    }

    void *scratch = malloc(size);
    if (scratch != NULL) {
        ++s_test_scratch_allocation_count;
    }
    return scratch;
}

void app_pod_status_bridge_test_fail_next_scratch_allocations(unsigned count)
{
    s_test_scratch_allocations_to_fail = count;
}

unsigned app_pod_status_bridge_test_scratch_allocation_attempts(void)
{
    return s_test_scratch_allocation_attempts;
}

unsigned app_pod_status_bridge_test_scratch_allocation_count(void)
{
    return s_test_scratch_allocation_count;
}
#endif


static void bridge_scratch_release(void)
{
    if (s_scratch == NULL) {
        s_scratch_allocation_warning_logged = false;
        return;
    }

#if defined(ESP_PLATFORM)
    heap_caps_free(s_scratch);
#else
    free(s_scratch);
#endif
    s_scratch = NULL;
    s_scratch_allocation_warning_logged = false;
}


static app_pod_status_bridge_scratch_t *bridge_scratch_get(void)
{
    if (s_scratch != NULL) {
        return s_scratch;
    }

#if defined(ESP_PLATFORM)
    app_pod_status_bridge_scratch_t *scratch =
        heap_caps_malloc(
            sizeof(*scratch),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (scratch == NULL) {
        scratch =
            heap_caps_malloc(
                sizeof(*scratch),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#else
    app_pod_status_bridge_scratch_t *scratch =
        bridge_scratch_allocate(sizeof(*scratch));
#endif

    if (scratch == NULL) {
        if (!s_scratch_allocation_warning_logged) {
            ESP_LOGW(
                TAG,
                "Unable to allocate publication scratch; local positions will be invalid");
            s_scratch_allocation_warning_logged = true;
        }
        return NULL;
    }

    memset(scratch, 0, sizeof(*scratch));
    s_scratch = scratch;
    s_scratch_allocation_warning_logged = false;
    return s_scratch;
}


static bool bridge_stop_requested(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}


static bool bridge_register_current_task(void)
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


static void bridge_clear_current_task(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_state_lock);
}


static void publish_channel_barometer(uint8_t channel)
{
    nrf_remote_sensor_cache_t *cache = s_scratch != NULL
        ? &s_scratch->sensor_cache
        : &s_sensor_cache_fallback;
    const esp_err_t err =
        nrf_topology_service_get_sensor_snapshot(channel, cache);

    const app_pod_status_barometer_reading_t reading =
        app_pod_status_bridge_compute_barometer_reading(
            err == ESP_OK ? cache : NULL);

    const device_sensor_health_t health = app_pod_status_sensor_health(
        err == ESP_OK ? cache : NULL, (uint64_t)(esp_timer_get_time() / 1000));
    (void)device_status_set_pod_sensor_health(channel, &health);

    (void)device_status_set_pod_barometer(
        channel,
        reading.pressure_pa,
        reading.relative_altitude_cm,
        APP_POD_STATUS_BRIDGE_REFERENCE_PRESSURE_PA,
        reading.valid && health.current);

    /* Measured values only, and only while the snapshot is fresh. */
    const app_pod_status_sensor_values_t values =
        app_pod_status_bridge_compute_sensor_values(err == ESP_OK ? cache : NULL);
    (void)device_status_set_pod_environment(
        channel,
        values.temperature_c,
        values.temperature_valid && health.current,
        values.humidity_percent,
        values.humidity_valid && health.current);
    (void)device_status_set_pod_gyro_rate(
        channel, values.gyro_rate_dps, values.gyro_valid && health.current);
}


static void publish_channel_height_reference(uint8_t channel)
{
    nrf_remote_sensor_cache_t *cache = s_scratch != NULL
        ? &s_scratch->sensor_cache
        : &s_sensor_cache_fallback;
    const esp_err_t err = nrf_topology_service_get_sensor_snapshot(channel, cache);
    device_height_reference_state_t state = DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED;
    uint16_t generation = 0U;
    int32_t relative_z_mm = 0;

    if (err == ESP_OK) {
        switch (cache->sensor.reference_state) {
            case NRF_UPSTREAM_REFERENCE_VALID:
                state = DEVICE_HEIGHT_REFERENCE_VALID;
                relative_z_mm = cache->sensor.relative_z_mm;
                break;
            case NRF_UPSTREAM_REFERENCE_REJECTED_MOVING:
                state = DEVICE_HEIGHT_REFERENCE_REJECTED_MOVING;
                break;
            case NRF_UPSTREAM_REFERENCE_SENSOR_INVALID:
                state = DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID;
                break;
            case NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED:
            default:
                break;
        }
        generation = cache->sensor.reference_generation;
    }

    (void)device_status_set_pod_height_reference(channel, state, generation, relative_z_mm);
}


/*
 * Fetches the currently active object's frozen Channel-Sounding-derived
 * geometry (MAX-118) at most once per cycle -- every pod shares the same
 * active object, so there is no reason to re-fetch per channel. `has_profile`
 * is false (and every pod therefore reports an invalid local_position) when
 * nothing is active, or the fetch fails; this deliberately never falls back
 * to a stale previously-active object's geometry.
 */
static void publish_channel_local_position(
    uint8_t channel,
    bool has_profile,
    const object_store_profile_t *profile
)
{
    const app_pod_status_local_position_reading_t reading =
        app_pod_status_bridge_compute_local_position_reading(
            has_profile ? &profile->geometry.pods[channel] : NULL);

    (void)device_status_set_pod_local_position(
        channel,
        reading.local_x_m,
        reading.local_y_m,
        reading.valid);
}


static void publish_pod_connection_states(void)
{
    nrf_topology_snapshot_t *topology = s_scratch != NULL
        ? &s_scratch->topology
        : &s_topology_fallback;
    if (nrf_topology_service_get_snapshot(topology) != ESP_OK) {
        return;
    }

    (void)app_pod_status_bridge_report_topology_inventory(topology);

    for (size_t channel = 0U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        (void)device_status_set_pod_state(
            channel,
            app_pod_status_bridge_map_topology_state(topology->remotes[channel].state));
    }
}


static void publish_cycle(void)
{
    app_pod_status_bridge_scratch_t *scratch = bridge_scratch_get();
    if (scratch != NULL) {
        memset(&scratch->profile, 0, sizeof(scratch->profile));
    }
    const bool has_profile = scratch != NULL &&
        object_store_get_active(&scratch->active) == ESP_OK &&
        scratch->active.active &&
        scratch->active.id[0] != '\0' &&
        object_store_get_profile_cached(scratch->active.id, &scratch->profile) == ESP_OK;

    publish_pod_connection_states();

    for (
        uint8_t channel = 0U;
        channel < DEVICE_STATUS_POD_COUNT;
        ++channel
    ) {
        publish_channel_height_reference(channel);
        publish_channel_barometer(channel);
        publish_channel_local_position(
            channel,
            has_profile,
            scratch != NULL ? &scratch->profile : NULL);
    }
}


void app_pod_status_bridge_publish_cycle(void)
{
    publish_cycle();
}


static void bridge_task(void *argument)
{
    (void)argument;

    const bool stop_before_start =
        bridge_register_current_task();

    while (!stop_before_start && !bridge_stop_requested()) {
        publish_cycle();

        const uint32_t notifications =
            ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(APP_POD_STATUS_BRIDGE_PERIOD_MS));

        if (notifications > 0U && bridge_stop_requested()) {
            break;
        }
    }

    bridge_scratch_release();
    bridge_clear_current_task();
    vTaskDeleteWithCaps(NULL);
}


esp_err_t app_pod_status_bridge_start(void)
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
    portEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t task = NULL;
    runtime_observability_log_memory("pod_bridge_before_task_create");
    const BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(
            bridge_task,
            "pod_status_bridge",
            APP_TASK_STACK_POD_STATUS_BRIDGE,
            NULL,
            APP_TASK_PRIORITY_POD_STATUS_BRIDGE,
            &task,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (result != pdPASS) {
        runtime_observability_log_memory("pod_bridge_task_create_failed");
        portENTER_CRITICAL(&s_state_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Pod barometer/local-position status bridge started on core=%d stack_bytes=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES, (unsigned)APP_TASK_STACK_POD_STATUS_BRIDGE);
    return ESP_OK;
}


void app_pod_status_bridge_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = true;
    const TaskHandle_t task = s_task;
    const bool starting = s_starting;
    portEXIT_CRITICAL(&s_state_lock);

    if (task == NULL && !starting) {
        bridge_scratch_release();
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
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
}
