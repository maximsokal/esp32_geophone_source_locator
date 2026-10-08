#include "web_server_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "measurement_service.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "storage_service.h"
#include "web_status_cache_arbitration.h"


typedef struct
{
    char object_id[OBJECT_STORE_ID_SIZE];
    seismic_recorder_session_summary_t sessions[SEISMIC_RECORDER_MAX_SESSIONS];
    size_t count;
    esp_err_t error;

} web_status_session_cache_t;


typedef struct
{
    object_store_profile_t profiles[OBJECT_STORE_MAX_OBJECTS];
    size_t profile_count;
    float defaults[OBJECT_STORE_POD_COUNT];
    web_status_session_cache_t session_cache[OBJECT_STORE_MAX_OBJECTS];

} web_status_cache_payload_t;


typedef enum
{
    WEB_STATUS_CACHE_COMMAND_REFRESH = 0,
    WEB_STATUS_CACHE_COMMAND_STOP

} web_status_cache_command_t;


static const char *TAG = "WEB_STATUS_CACHE";

static web_status_cache_payload_t *s_payload_a = NULL;
static web_status_cache_payload_t *s_payload_b = NULL;
static web_status_cache_payload_t *s_active_payload = NULL;
static web_status_cache_arbitration_t s_arbitration;
static SemaphoreHandle_t s_refresh_mutex = NULL;
static QueueHandle_t s_refresh_queue = NULL;
static TaskHandle_t s_refresh_task = NULL;
static char s_active_object_id[OBJECT_STORE_ID_SIZE];


static bool web_status_json_add_location(
    cJSON *parent,
    const object_store_location_t *location
)
{
    cJSON *object = cJSON_AddObjectToObject(parent, "location");
    if (object == NULL) return false;

    return
        cJSON_AddBoolToObject(object, "valid", location->valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", location->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", location->longitude) != NULL &&
        cJSON_AddNumberToObject(object, "accuracy_m", location->accuracy_m) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", location->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", location->altitude_m) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_accuracy_m", location->altitude_accuracy_m) != NULL &&
        cJSON_AddNumberToObject(object, "captured_at_ms", (double)location->captured_at_ms) != NULL &&
        cJSON_AddStringToObject(object, "source", location->source) != NULL;
}


static web_status_cache_payload_t *web_status_cache_payload_allocate(void)
{
    web_status_cache_payload_t *payload = heap_caps_calloc(
        1U,
        sizeof(*payload),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (payload == NULL) {
        ESP_LOGW(
            TAG,
            "PSRAM object/session cache allocation failed; falling back to default heap bytes=%u",
            (unsigned)sizeof(*payload)
        );
        payload = calloc(1U, sizeof(*payload));
    }

    return payload;
}


static esp_err_t web_status_cache_payloads_ensure(void)
{
    if (s_payload_a != NULL && s_payload_b != NULL) {
        return ESP_OK;
    }

    if (s_payload_a == NULL) {
        s_payload_a = web_status_cache_payload_allocate();
    }
    if (s_payload_b == NULL) {
        s_payload_b = web_status_cache_payload_allocate();
    }

    if (s_payload_a == NULL || s_payload_b == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "WEB_STATUS_CACHE storage=double-buffer bytes=%u source=psram-preferred",
        (unsigned)(sizeof(*s_payload_a) * 2U)
    );
    return ESP_OK;
}


static bool web_status_cache_storage_is_idle(void *user_context)
{
    (void)user_context;

    measurement_service_snapshot_t measurement;
    if (measurement_service_get_snapshot(&measurement) == ESP_OK) {
        if (
            measurement.state == MEASUREMENT_SERVICE_STARTING ||
            measurement.state == MEASUREMENT_SERVICE_RECORDING ||
            measurement.state == MEASUREMENT_SERVICE_STOPPING ||
            measurement.state == MEASUREMENT_SERVICE_DELETING
        ) {
            return false;
        }

        /* The measurement worker keeps its command gate held while invoking
         * the post-STOP/DELETE callback. State and the recorder snapshot are
         * the storage-safety boundary; the gate itself is not. */
    }

    seismic_recorder_snapshot_t recorder;
    if (seismic_recorder_get_snapshot(&recorder) == ESP_OK) {
        if (
            recorder.running ||
            recorder.state == SEISMIC_RECORDER_STOPPING
        ) {
            return false;
        }
    }

    return true;
}


static esp_err_t web_status_cache_publish_result(
    web_status_cache_payload_t *payload,
    esp_err_t error
)
{
    esp_err_t publish_error = error;
    object_store_summary_t active_object;
    bool active_available = false;

    /* ACTIVE is independent object-store authority. Never infer it solely
     * from which profile cards happened to survive a bounded/cache refresh. */
    if (publish_error == ESP_OK) {
        const esp_err_t active_err =
            object_store_get_active(&active_object);
        if (active_err == ESP_OK) {
            active_available = true;
        }
        else if (active_err != ESP_ERR_NOT_FOUND) {
            publish_error = active_err;
        }
    }

    if (!web_status_cache_arbitration_lock(&s_arbitration)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (publish_error == ESP_OK) {
        s_active_payload = payload;
        if (active_available) {
            snprintf(
                s_active_object_id,
                sizeof(s_active_object_id),
                "%s",
                active_object.id
            );
        }
        else {
            s_active_object_id[0] = '\0';
        }
    }

    web_status_cache_arbitration_unlock(&s_arbitration);
    web_status_cache_arbitration_finish_refresh(
        &s_arbitration,
        publish_error == ESP_OK
    );
    return publish_error;
}


esp_err_t web_status_json_objects_refresh(void)
{
    if (s_arbitration.mutex == NULL || s_refresh_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_refresh_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = web_status_cache_payloads_ensure();
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_refresh_mutex);
        return err;
    }

    /* The arbitration state is reserved before any object/session filesystem
     * call. START uses the same reservation, so its guard cannot pass in the
     * gap between an idle check and the first SD read. */
    if (!web_status_cache_arbitration_try_reserve_refresh(
        &s_arbitration,
        web_status_cache_storage_is_idle,
        NULL
    )) {
        (void)xSemaphoreGive(s_refresh_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!web_status_cache_arbitration_lock(&s_arbitration)) {
        web_status_cache_arbitration_finish_refresh(&s_arbitration, false);
        (void)xSemaphoreGive(s_refresh_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    web_status_cache_payload_t *staging =
        s_active_payload == s_payload_a ? s_payload_b : s_payload_a;
    web_status_cache_arbitration_unlock(&s_arbitration);

    memset(staging, 0, sizeof(*staging));
    const int64_t started_us = esp_timer_get_time();
    size_t profile_count = 0U;

    /*
     * Status refresh is low-priority background work. The responsive object
     * snapshot deliberately releases storage_manager (and therefore the TFT
     * quiesce guard) between object.json files instead of keeping SPI2 owned
     * for the whole directory walk.
     */
    err = object_store_list_profiles_responsive(
        staging->profiles,
        OBJECT_STORE_MAX_OBJECTS,
        &profile_count,
        staging->defaults
    );

    size_t session_errors = 0U;
    if (err == ESP_OK) {
        staging->profile_count = profile_count;
        for (size_t i = 0U; i < profile_count; ++i) {
            web_status_session_cache_t *session_cache =
                &staging->session_cache[i];
            snprintf(
                session_cache->object_id,
                sizeof(session_cache->object_id),
                "%s",
                staging->profiles[i].summary.id
            );

            /*
             * Session history uses the same cooperative rule: one parent
             * census and then one bounded metadata window per session. This
             * prevents the first Web cache refresh from pinning LVGL behind a
             * multi-second storage operation.
             */
            session_cache->error = seismic_recorder_list_sessions_responsive(
                session_cache->object_id,
                session_cache->sessions,
                SEISMIC_RECORDER_MAX_SESSIONS,
                &session_cache->count
            );
            if (session_cache->error != ESP_OK) {
                session_cache->count = 0U;
                ++session_errors;
                ESP_LOGW(
                    TAG,
                    "Session cache refresh failed object=%s err=%s",
                    session_cache->object_id,
                    esp_err_to_name(session_cache->error)
                );
            }
        }
    }

    err = web_status_cache_publish_result(staging, err);

    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "WEB_STATUS_CACHE refresh=done objects=%u session_errors=%u elapsed_ms=%lld mode=cooperative",
            (unsigned)profile_count,
            (unsigned)session_errors,
            elapsed_us / 1000LL
        );
    }
    else {
        ESP_LOGW(
            TAG,
            "WEB_STATUS_CACHE refresh=failed err=%s elapsed_ms=%lld mode=cooperative",
            esp_err_to_name(err),
            elapsed_us / 1000LL
        );
    }

    (void)xSemaphoreGive(s_refresh_mutex);
    return err;
}


static void web_status_cache_refresh_task(void *argument)
{
    (void)argument;

    for (;;) {
        web_status_cache_command_t command;
        if (xQueueReceive(s_refresh_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (command == WEB_STATUS_CACHE_COMMAND_STOP) {
            break;
        }

        const esp_err_t err = web_status_json_objects_refresh();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(
                TAG,
                "WEB_STATUS_CACHE async refresh failed: %s",
                esp_err_to_name(err)
            );
        }
    }

    s_refresh_task = NULL;
    vTaskDeleteWithCaps(NULL);
}


esp_err_t web_status_json_objects_init(void)
{
    if (s_arbitration.mutex != NULL) {
        return storage_service_set_content_changed_observer(
            web_status_json_objects_storage_changed,
            NULL
        );
    }

    esp_err_t err = web_status_cache_arbitration_init(&s_arbitration);
    if (err != ESP_OK) {
        return err;
    }

    s_refresh_mutex = xSemaphoreCreateMutex();
    s_refresh_queue = xQueueCreate(1U, sizeof(web_status_cache_command_t));
    if (
        s_refresh_mutex == NULL ||
        s_refresh_queue == NULL
    ) {
        if (s_refresh_queue != NULL) vQueueDelete(s_refresh_queue);
        if (s_refresh_mutex != NULL) vSemaphoreDelete(s_refresh_mutex);
        s_refresh_queue = NULL;
        s_refresh_mutex = NULL;
        web_status_cache_arbitration_shutdown(&s_arbitration);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        web_status_cache_refresh_task,
        "web_status_cache",
        APP_TASK_STACK_WEB_STATUS_CACHE,
        NULL,
        APP_TASK_PRIORITY_WEB_STATUS_CACHE,
        &s_refresh_task,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (result != pdPASS) {
        vQueueDelete(s_refresh_queue);
        vSemaphoreDelete(s_refresh_mutex);
        s_refresh_queue = NULL;
        s_refresh_mutex = NULL;
        web_status_cache_arbitration_shutdown(&s_arbitration);
        return ESP_ERR_NO_MEM;
    }

    err = storage_service_set_content_changed_observer(
        web_status_json_objects_storage_changed,
        NULL
    );
    if (err != ESP_OK) {
        const web_status_cache_command_t command =
            WEB_STATUS_CACHE_COMMAND_STOP;
        (void)xQueueSend(s_refresh_queue, &command, portMAX_DELAY);
        while (s_refresh_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vQueueDelete(s_refresh_queue);
        vSemaphoreDelete(s_refresh_mutex);
        s_refresh_queue = NULL;
        s_refresh_mutex = NULL;
        web_status_cache_arbitration_shutdown(&s_arbitration);
        return err;
    }

    ESP_LOGI(
        TAG,
        "WEB_STATUS_CACHE initialized core=%d stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        APP_TASK_STACK_WEB_STATUS_CACHE
    );
    return ESP_OK;
}


void web_status_json_objects_shutdown(void)
{
    /* Unregister first. The storage-service setter waits for any callback that
     * already snapped this cache context before payload/queue memory is freed. */
    (void)storage_service_set_content_changed_observer(NULL, NULL);

    if (s_refresh_task != NULL && s_refresh_queue != NULL) {
        const web_status_cache_command_t command =
            WEB_STATUS_CACHE_COMMAND_STOP;
        (void)xQueueSend(s_refresh_queue, &command, portMAX_DELAY);
        while (s_refresh_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_refresh_mutex != NULL) {
        (void)xSemaphoreTake(s_refresh_mutex, portMAX_DELAY);
        (void)xSemaphoreGive(s_refresh_mutex);
    }

    heap_caps_free(s_payload_a);
    heap_caps_free(s_payload_b);
    s_payload_a = NULL;
    s_payload_b = NULL;
    s_active_payload = NULL;
    s_active_object_id[0] = '\0';

    if (s_refresh_queue != NULL) vQueueDelete(s_refresh_queue);
    if (s_refresh_mutex != NULL) vSemaphoreDelete(s_refresh_mutex);
    s_refresh_queue = NULL;
    s_refresh_mutex = NULL;
    web_status_cache_arbitration_shutdown(&s_arbitration);
}


esp_err_t web_status_json_objects_request_refresh(void)
{
    if (s_refresh_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const web_status_cache_command_t command =
        WEB_STATUS_CACHE_COMMAND_REFRESH;
    return xQueueOverwrite(s_refresh_queue, &command) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}


void web_status_json_objects_invalidate(void)
{
    web_status_cache_arbitration_invalidate(&s_arbitration);
}


bool web_status_json_objects_cache_ready(void)
{
    return web_status_cache_arbitration_cache_ready(&s_arbitration);
}


bool web_status_json_objects_cache_busy(void)
{
    return !web_status_json_objects_cache_ready();
}


esp_err_t web_status_json_objects_start_guard(void *user_context)
{
    (void)user_context;
    /* The status cache is auxiliary. If initialization failed completely,
     * there is no refresh operation that can race local TFT recording, so the
     * measurement service must remain usable without this guard. */
    if (s_arbitration.mutex == NULL) {
        return ESP_OK;
    }
    return web_status_cache_arbitration_try_reserve_start(&s_arbitration)
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}


void web_status_json_objects_release_start_reservation(void *user_context)
{
    (void)user_context;
    web_status_cache_arbitration_release_start(&s_arbitration);
}


void web_status_json_objects_storage_changed(void *user_context)
{
    (void)user_context;
    web_status_json_objects_invalidate();
    const esp_err_t err = web_status_json_objects_request_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "WEB_STATUS_CACHE post-storage refresh request failed: %s",
            esp_err_to_name(err)
        );
    }
}


void web_status_json_objects_object_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;
    if (event == NULL) {
        return;
    }

    if (event->type == OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE) {
        web_status_json_objects_set_active(event->object_id);
        return;
    }

    if (
        event->type != OBJECT_SERVICE_EVENT_CREATE_DONE &&
        event->type != OBJECT_SERVICE_EVENT_CREATE_FAILED &&
        event->type != OBJECT_SERVICE_EVENT_DELETE_DONE &&
        event->type != OBJECT_SERVICE_EVENT_DELETE_FAILED
    ) {
        /* LIST events only report a read; keep the prefetched payload usable
         * during RECORDING instead of turning a UI list refresh into 503. */
        return;
    }

    web_status_json_objects_invalidate();
    const esp_err_t err = web_status_json_objects_request_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "WEB_STATUS_CACHE object event refresh request failed type=%d err=%s",
            (int)event->type,
            esp_err_to_name(err)
        );
    }
}


static bool web_status_cache_payload_contains_object_id(
    const web_status_cache_payload_t *payload,
    const char *object_id
)
{
    if (payload == NULL || object_id == NULL || object_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0U; i < payload->profile_count; ++i) {
        if (strcmp(payload->profiles[i].summary.id, object_id) == 0) {
            return true;
        }
    }
    return false;
}


void web_status_json_objects_set_active(const char *object_id)
{
    if (!web_status_cache_arbitration_lock(&s_arbitration)) {
        return;
    }

    const bool object_present =
        web_status_cache_payload_contains_object_id(
            s_active_payload,
            object_id
        );
    const bool refresh_required =
        web_status_active_membership_requires_refresh(
            s_arbitration.cache_ready,
            s_arbitration.cache_refreshing,
            s_active_payload != NULL,
            object_present
        );

    /* If the target is already a member of a stable bounded snapshot, only
     * ACTIVE changed; JSON derives each card's active flag from this authority
     * and the cached object/session membership remains valid. */
    if (!refresh_required) {
        snprintf(
            s_active_object_id,
            sizeof(s_active_object_id),
            "%s",
            object_id != NULL ? object_id : ""
        );
        web_status_cache_arbitration_unlock(&s_arbitration);
        return;
    }

    /* Keep the old id while the cache is still publishable. The next step
     * invalidates first, so no reader can observe a new ACTIVE id paired with
     * the old bounded objects[] membership. */
    web_status_cache_arbitration_unlock(&s_arbitration);
    web_status_json_objects_invalidate();

    if (web_status_cache_arbitration_lock(&s_arbitration)) {
        snprintf(
            s_active_object_id,
            sizeof(s_active_object_id),
            "%s",
            object_id != NULL ? object_id : ""
        );
        web_status_cache_arbitration_unlock(&s_arbitration);
    }

    const esp_err_t err = web_status_json_objects_request_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "WEB_STATUS_CACHE active membership refresh request failed: %s",
            esp_err_to_name(err)
        );
    }
}


static bool web_status_json_add_objects_from_payload(
    cJSON *root,
    const web_status_cache_payload_t *payload
)
{
    cJSON *store = cJSON_AddObjectToObject(root, "object_store");
    if (store == NULL) return false;

    if (cJSON_AddBoolToObject(store, "available", true) == NULL) {
        return false;
    }
    if (s_active_object_id[0] != '\0') {
        if (cJSON_AddStringToObject(store, "active_id", s_active_object_id) == NULL) {
            return false;
        }
    }
    else if (cJSON_AddNullToObject(store, "active_id") == NULL) {
        return false;
    }

    cJSON *default_array = cJSON_AddArrayToObject(store, "default_cable_lengths_m");
    cJSON *objects = cJSON_AddArrayToObject(store, "objects");
    if (default_array == NULL || objects == NULL) {
        return false;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *number = cJSON_CreateNumber(payload->defaults[i]);
        if (number == NULL) return false;
        if (!cJSON_AddItemToArray(default_array, number)) {
            cJSON_Delete(number);
            return false;
        }
    }

    for (size_t i = 0U; i < payload->profile_count; ++i) {
        const object_store_profile_t *profile = &payload->profiles[i];
        const bool active = s_active_object_id[0] != '\0'
            ? strcmp(profile->summary.id, s_active_object_id) == 0
            : profile->summary.active;

        cJSON *object = cJSON_CreateObject();
        if (object == NULL) return false;
        if (!cJSON_AddItemToArray(objects, object)) {
            cJSON_Delete(object);
            return false;
        }

        if (
            cJSON_AddStringToObject(object, "id", profile->summary.id) == NULL ||
            cJSON_AddStringToObject(object, "name", profile->summary.name) == NULL ||
            cJSON_AddBoolToObject(object, "active", active) == NULL ||
            !web_status_json_add_location(object, &profile->location)
        ) {
            return false;
        }

        cJSON *cables = cJSON_AddArrayToObject(object, "cable_lengths_m");
        if (cables == NULL) return false;
        for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
            cJSON *number = cJSON_CreateNumber(profile->cable_lengths_m[pod]);
            if (number == NULL) return false;
            if (!cJSON_AddItemToArray(cables, number)) {
                cJSON_Delete(number);
                return false;
            }
        }
    }

    return true;
}


esp_err_t web_status_json_add_objects(cJSON *root)
{
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!web_status_cache_arbitration_lock(&s_arbitration)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        !s_arbitration.cache_ready ||
        s_arbitration.cache_refreshing ||
        s_active_payload == NULL
    ) {
        web_status_cache_arbitration_unlock(&s_arbitration);
        return ESP_ERR_INVALID_STATE;
    }

    const bool ok = web_status_json_add_objects_from_payload(
        root,
        s_active_payload
    );
    web_status_cache_arbitration_unlock(&s_arbitration);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}


static const web_status_session_cache_t *web_status_find_session_cache(
    const web_status_cache_payload_t *payload,
    const char *object_id
)
{
    for (size_t i = 0U; i < payload->profile_count; ++i) {
        if (strcmp(payload->session_cache[i].object_id, object_id) == 0) {
            return &payload->session_cache[i];
        }
    }
    return NULL;
}


esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id)
{
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (object_id == NULL || object_id[0] == '\0') {
        return ESP_OK;
    }
    if (!web_status_cache_arbitration_lock(&s_arbitration)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        !s_arbitration.cache_ready ||
        s_arbitration.cache_refreshing ||
        s_active_payload == NULL
    ) {
        web_status_cache_arbitration_unlock(&s_arbitration);
        return ESP_ERR_INVALID_STATE;
    }

    const web_status_session_cache_t *session_cache =
        web_status_find_session_cache(s_active_payload, object_id);

    cJSON *collection = cJSON_AddObjectToObject(root, "recording_sessions");
    if (collection == NULL) {
        goto no_mem;
    }
    if (cJSON_AddStringToObject(collection, "object_id", object_id) == NULL) {
        goto no_mem;
    }

    const esp_err_t error = session_cache == NULL
        ? ESP_ERR_NOT_FOUND
        : session_cache->error;
    if (cJSON_AddBoolToObject(collection, "available", error == ESP_OK) == NULL) {
        goto no_mem;
    }

    cJSON *array = cJSON_AddArrayToObject(collection, "sessions");
    if (array == NULL) {
        goto no_mem;
    }

    if (error != ESP_OK) {
        if (cJSON_AddStringToObject(collection, "error", esp_err_to_name(error)) == NULL) {
            goto no_mem;
        }
        web_status_cache_arbitration_unlock(&s_arbitration);
        return ESP_OK;
    }

    for (size_t i = 0U; i < session_cache->count; ++i) {
        const seismic_recorder_session_summary_t *summary =
            &session_cache->sessions[i];
        cJSON *session = cJSON_CreateObject();
        if (session == NULL) {
            goto no_mem;
        }
        if (!cJSON_AddItemToArray(array, session)) {
            cJSON_Delete(session);
            goto no_mem;
        }

        if (
            cJSON_AddStringToObject(session, "session_id", summary->session_id) == NULL ||
            cJSON_AddStringToObject(
                session,
                "status",
                seismic_recorder_session_status_name(summary->status)
            ) == NULL ||
            cJSON_AddBoolToObject(session, "complete", summary->complete) == NULL ||
            cJSON_AddBoolToObject(session, "readable", summary->readable) == NULL ||
            cJSON_AddBoolToObject(session, "recovered", summary->recovered) == NULL ||
            cJSON_AddStringToObject(session, "source", summary->source_name) == NULL ||
            cJSON_AddNumberToObject(session, "sample_rate_hz", summary->sample_rate_hz) == NULL ||
            cJSON_AddNumberToObject(session, "channel_count", summary->channel_count) == NULL ||
            cJSON_AddNumberToObject(session, "samples_written", (double)summary->samples_written) == NULL ||
            cJSON_AddBoolToObject(
                session,
                "dropped_frames_available",
                summary->dropped_frames_available
            ) == NULL ||
            cJSON_AddNumberToObject(session, "dropped_frames", (double)summary->dropped_frames) == NULL ||
            cJSON_AddBoolToObject(
                session,
                "recording_ms_available",
                summary->recording_ms_available
            ) == NULL ||
            cJSON_AddNumberToObject(session, "recording_ms", (double)summary->recording_ms) == NULL
        ) {
            goto no_mem;
        }
    }

    web_status_cache_arbitration_unlock(&s_arbitration);
    return ESP_OK;

no_mem:
    web_status_cache_arbitration_unlock(&s_arbitration);
    return ESP_ERR_NO_MEM;
}
