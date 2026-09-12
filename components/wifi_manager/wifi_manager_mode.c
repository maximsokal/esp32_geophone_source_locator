#include "wifi_manager.h"

#include "app_events.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_runtime.h"
#include "wifi_manager_sta.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"


static const char *TAG =
    "WIFI_MANAGER";

static portMUX_TYPE s_radio_stop_callback_lock =
    portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_radio_stop_prepare_callback_t s_radio_stop_prepare_callback =
    NULL;
static wifi_manager_radio_stop_complete_callback_t s_radio_stop_complete_callback =
    NULL;
static void *s_radio_stop_prepare_context =
    NULL;
static unsigned s_radio_stop_lifecycle_in_flight = 0U;
static wifi_manager_mode_transition_complete_callback_t
    s_mode_transition_complete_callback = NULL;
static void *s_mode_transition_complete_context = NULL;
static unsigned s_mode_transition_callback_in_flight = 0U;


esp_err_t wifi_manager_set_radio_stop_lifecycle_callbacks(
    wifi_manager_radio_stop_prepare_callback_t prepare_callback,
    wifi_manager_radio_stop_complete_callback_t complete_callback,
    void *context
)
{
    if (
        prepare_callback == NULL &&
        (complete_callback != NULL || context != NULL)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    s_radio_stop_prepare_callback = prepare_callback;
    s_radio_stop_complete_callback = complete_callback;
    s_radio_stop_prepare_context = context;
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (prepare_callback == NULL) {
        for (;;) {
            unsigned in_flight;
            portENTER_CRITICAL(&s_radio_stop_callback_lock);
            in_flight = s_radio_stop_lifecycle_in_flight;
            portEXIT_CRITICAL(&s_radio_stop_callback_lock);
            if (in_flight == 0U) break;
            vTaskDelay(1U);
        }
    }

    return ESP_OK;
}


esp_err_t wifi_manager_set_radio_stop_prepare_callback(
    wifi_manager_radio_stop_prepare_callback_t callback,
    void *context
)
{
    return wifi_manager_set_radio_stop_lifecycle_callbacks(
        callback,
        NULL,
        context
    );
}


esp_err_t wifi_manager_set_mode_transition_complete_callback(
    wifi_manager_mode_transition_complete_callback_t callback,
    void *context
)
{
    if (callback == NULL && context != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    s_mode_transition_complete_callback = callback;
    s_mode_transition_complete_context = context;
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (callback == NULL) {
        for (;;) {
            unsigned in_flight;
            portENTER_CRITICAL(&s_radio_stop_callback_lock);
            in_flight = s_mode_transition_callback_in_flight;
            portEXIT_CRITICAL(&s_radio_stop_callback_lock);
            if (in_flight == 0U) break;
            vTaskDelay(1U);
        }
    }

    return ESP_OK;
}


static void wifi_manager_notify_mode_transition_complete(
    bool running,
    bool setup_mode,
    esp_err_t transition_result,
    esp_err_t cleanup_result
)
{
    wifi_manager_mode_transition_complete_callback_t callback = NULL;
    void *context = NULL;

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    callback = s_mode_transition_complete_callback;
    context = s_mode_transition_complete_context;
    if (callback != NULL) {
        ++s_mode_transition_callback_in_flight;
    }
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (callback == NULL) {
        return;
    }

    callback(
        context,
        running,
        setup_mode,
        transition_result,
        cleanup_result
    );

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    --s_mode_transition_callback_in_flight;
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);
}


static esp_err_t wifi_manager_abort_mode_transition(
    esp_err_t transition_error
)
{
    /* Do not let a failed target start leave a partially started radio able
     * to reconnect after the manager has declared the transition failed. */
    wifi_manager_sta_reset_state();
    wifi_manager_runtime_clear_setup_client();

    const esp_err_t cleanup_err = esp_wifi_stop();
    const esp_err_t cleanup_result =
        cleanup_err == ESP_ERR_WIFI_NOT_STARTED
            ? ESP_OK
            : cleanup_err;
    if (cleanup_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Wi-Fi target start failed: target=%s cleanup=%s; radio state is uncertain",
            esp_err_to_name(transition_error),
            esp_err_to_name(cleanup_result)
        );
    }
    else {
        ESP_LOGW(
            TAG,
            "Wi-Fi target start failed: %s; using stopped/disconnected fallback",
            esp_err_to_name(transition_error)
        );
    }

    /* The old mode is already gone. A failed cleanup cannot be reported as a
     * clean stop, even though the public running/setup flags stay false. */
    wifi_manager_internal_set_radio_state_uncertain(
        cleanup_result != ESP_OK
    );
    wifi_manager_internal_set_running(false);
    wifi_manager_internal_set_setup_mode(false);
    wifi_manager_notify_mode_transition_complete(
        false,
        false,
        transition_error,
        cleanup_result
    );
    return transition_error;
}


static esp_err_t wifi_manager_recover_uncertain_radio_state(void)
{
    wifi_manager_sta_reset_state();
    wifi_manager_runtime_clear_setup_client();

    const esp_err_t cleanup_err = esp_wifi_stop();
    const esp_err_t cleanup_result =
        cleanup_err == ESP_ERR_WIFI_NOT_STARTED
            ? ESP_OK
            : cleanup_err;
    if (cleanup_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Wi-Fi uncertain-state cleanup retry failed: %s",
            esp_err_to_name(cleanup_result)
        );
        wifi_manager_internal_set_radio_state_uncertain(true);
        wifi_manager_internal_set_running(false);
        wifi_manager_internal_set_setup_mode(false);
        wifi_manager_notify_mode_transition_complete(
            false,
            false,
            ESP_OK,
            cleanup_result
        );
        return cleanup_result;
    }

    wifi_manager_internal_set_radio_state_uncertain(false);
    wifi_manager_internal_set_running(false);
    wifi_manager_internal_set_setup_mode(false);
    wifi_manager_notify_mode_transition_complete(
        false,
        false,
        ESP_OK,
        ESP_OK
    );
    return ESP_OK;
}


static esp_err_t wifi_manager_prepare_radio_stop(
    wifi_manager_radio_stop_prepare_callback_t *prepare_callback,
    wifi_manager_radio_stop_complete_callback_t *complete_callback,
    void **context
)
{
    if (
        prepare_callback == NULL ||
        complete_callback == NULL ||
        context == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    *prepare_callback = s_radio_stop_prepare_callback;
    *complete_callback = s_radio_stop_complete_callback;
    *context = s_radio_stop_prepare_context;
    if (*prepare_callback != NULL) {
        ++s_radio_stop_lifecycle_in_flight;
    }
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (*prepare_callback == NULL) {
        return ESP_OK;
    }

    const esp_err_t err = (*prepare_callback)(*context);

    return err;
}


static esp_err_t wifi_manager_stop_radio(void)
{
    /*
     * Network services own their lifecycle. The prepare callback is a
     * synchronous barrier: it must return only after those services have
     * released resources that must not survive the Wi-Fi mode transition.
     */
    wifi_manager_radio_stop_prepare_callback_t prepare_callback = NULL;
    wifi_manager_radio_stop_complete_callback_t complete_callback = NULL;
    void *context = NULL;
    esp_err_t err =
        wifi_manager_prepare_radio_stop(
            &prepare_callback,
            &complete_callback,
            &context
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Network lifecycle did not quiesce before radio stop: %s",
            esp_err_to_name(err)
        );
        if (prepare_callback != NULL) {
            portENTER_CRITICAL(&s_radio_stop_callback_lock);
            --s_radio_stop_lifecycle_in_flight;
            portEXIT_CRITICAL(&s_radio_stop_callback_lock);
        }
        return err;
    }

    wifi_manager_sta_reset_state();

    err =
        esp_wifi_stop();

    const esp_err_t radio_stop_result =
        err == ESP_ERR_WIFI_NOT_STARTED
            ? ESP_OK
            : err;

    if (complete_callback != NULL) {
        complete_callback(context, radio_stop_result);
    }

    if (prepare_callback != NULL) {
        portENTER_CRITICAL(&s_radio_stop_callback_lock);
        --s_radio_stop_lifecycle_in_flight;
        portEXIT_CRITICAL(&s_radio_stop_callback_lock);
    }

    if (
        radio_stop_result == ESP_OK
    ) {
        return ESP_OK;
    }

    return radio_stop_result;
}


esp_err_t wifi_manager_enter_setup_mode(void)
{
    if (
        !wifi_manager_internal_initialized()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_manager_internal_radio_state_uncertain()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        wifi_manager_internal_setup_mode()
    ) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_internal_set_running(false);
    wifi_manager_internal_set_setup_mode(false);
    wifi_manager_runtime_clear_setup_client();

    err =
        wifi_manager_internal_start_setup_mode();

    if (err == ESP_OK) {
        wifi_manager_internal_set_radio_state_uncertain(false);
        wifi_manager_internal_set_running(
            true
        );
        wifi_manager_notify_mode_transition_complete(
            true,
            true,
            ESP_OK,
            ESP_OK
        );
    }
    else {
        return wifi_manager_abort_mode_transition(err);
    }

    return err;
}


esp_err_t wifi_manager_connect_saved(void)
{
    if (
        !wifi_manager_internal_initialized()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_manager_internal_radio_state_uncertain()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_internal_set_running(false);
    wifi_manager_internal_set_setup_mode(false);
    wifi_manager_runtime_clear_setup_client();

    err =
        wifi_manager_sta_start();

    if (err != ESP_OK) {
        return wifi_manager_abort_mode_transition(err);
    }

    wifi_manager_internal_set_running(
        true
    );
    wifi_manager_internal_set_radio_state_uncertain(false);
    wifi_manager_notify_mode_transition_complete(
        true,
        false,
        ESP_OK,
        ESP_OK
    );

    ESP_LOGI(
        TAG,
        "Switching to saved Wi-Fi STA credentials"
    );

    (void)app_events_post(
        APP_EVENT_WIFI_STA_CONNECTING,
        0
    );

    return ESP_OK;
}


esp_err_t wifi_manager_stop(void)
{
    if (wifi_manager_internal_radio_state_uncertain()) {
        return wifi_manager_recover_uncertain_radio_state();
    }

    if (
        !wifi_manager_is_running()
    ) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_runtime_clear_setup_client();

    wifi_manager_internal_set_running(
        false
    );

    wifi_manager_internal_set_setup_mode(
        false
    );
    wifi_manager_internal_set_radio_state_uncertain(false);

    wifi_manager_notify_mode_transition_complete(
        false,
        false,
        ESP_OK,
        ESP_OK
    );

    return ESP_OK;
}
