#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app_event_bus.h"
#include "freertos/FreeRTOS.h"
#include "wifi_manager.h"

enum {
    TRACE_PREPARE = 1,
    TRACE_MDNS_STOP,
    TRACE_HTTPS_STOP,
    TRACE_STA_RESET,
    TRACE_WIFI_STOP,
    TRACE_COMPLETE,
};

static int trace[8];
static size_t trace_count;
static bool fake_running;
static bool fake_setup_mode;
static bool fake_radio_state_uncertain;
static bool fake_mdns_ready;
static bool fake_https_running;
static esp_err_t prepare_result;
static esp_err_t fake_wifi_stop_result;
static esp_err_t fake_cleanup_wifi_stop_result;
static size_t fake_wifi_stop_calls;
static esp_err_t fake_setup_start_result;
static esp_err_t fake_sta_start_result;
static esp_err_t complete_result;
static size_t mode_transition_complete_calls;
static bool mode_transition_running;
static bool mode_transition_setup_mode;
static esp_err_t mode_transition_result;
static esp_err_t mode_transition_cleanup_result;

static void reset_trace(void)
{
    trace_count = 0U;
    fake_wifi_stop_calls = 0U;
}

static esp_err_t prepare_radio_stop(void *context)
{
    (void)context;
    trace[trace_count++] = TRACE_PREPARE;

    if (prepare_result != ESP_OK) {
        return prepare_result;
    }

    fake_mdns_ready = false;
    trace[trace_count++] = TRACE_MDNS_STOP;
    fake_https_running = false;
    trace[trace_count++] = TRACE_HTTPS_STOP;
    return prepare_result;
}

static void complete_radio_stop(
    void *context,
    esp_err_t radio_stop_result
)
{
    (void)context;
    complete_result = radio_stop_result;
    trace[trace_count++] = TRACE_COMPLETE;
}

static void complete_mode_transition(
    void *context,
    bool running,
    bool setup_mode,
    esp_err_t transition_result,
    esp_err_t cleanup_result
)
{
    (void)context;
    ++mode_transition_complete_calls;
    mode_transition_running = running;
    mode_transition_setup_mode = setup_mode;
    mode_transition_result = transition_result;
    mode_transition_cleanup_result = cleanup_result;
}

bool wifi_manager_is_running(void)
{
    return fake_running;
}

bool wifi_manager_internal_initialized(void)
{
    return true;
}

void wifi_manager_internal_set_running(bool running)
{
    fake_running = running;
}

void wifi_manager_internal_set_radio_state_uncertain(bool uncertain)
{
    fake_radio_state_uncertain = uncertain;
}

void wifi_manager_internal_set_setup_mode(bool setup_mode)
{
    fake_setup_mode = setup_mode;
}

bool wifi_manager_internal_setup_mode(void)
{
    return fake_setup_mode;
}

bool wifi_manager_internal_radio_state_uncertain(void)
{
    return fake_radio_state_uncertain;
}

esp_err_t wifi_manager_internal_start_setup_mode(void)
{
    if (fake_setup_start_result != ESP_OK) {
        return fake_setup_start_result;
    }

    fake_setup_mode = true;
    return ESP_OK;
}

void wifi_manager_runtime_clear_setup_client(void)
{
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t wifi_manager_sta_start(void)
{
    if (fake_sta_start_result != ESP_OK) {
        return fake_sta_start_result;
    }

    return ESP_OK;
}

void wifi_manager_sta_reset_state(void)
{
    trace[trace_count++] = TRACE_STA_RESET;
}

esp_err_t esp_wifi_stop(void)
{
    trace[trace_count++] = TRACE_WIFI_STOP;
    const esp_err_t result =
        fake_wifi_stop_calls++ == 0U
            ? fake_wifi_stop_result
            : fake_cleanup_wifi_stop_result;
    return result;
}

esp_err_t app_event_bus_publish(app_event_type_t type, int32_t value)
{
    (void)type;
    (void)value;
    return ESP_OK;
}

int main(void)
{
    fake_running = true;
    fake_setup_mode = false;
    fake_radio_state_uncertain = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_cleanup_wifi_stop_result = ESP_OK;
    fake_setup_start_result = ESP_OK;
    fake_sta_start_result = ESP_OK;
    complete_result = ESP_FAIL;
    mode_transition_complete_calls = 0U;
    mode_transition_running = false;
    mode_transition_setup_mode = false;
    mode_transition_result = ESP_FAIL;
    mode_transition_cleanup_result = ESP_FAIL;
    reset_trace();

    assert(wifi_manager_set_radio_stop_lifecycle_callbacks(
        prepare_radio_stop,
        complete_radio_stop,
        NULL
    ) == ESP_OK);
    assert(wifi_manager_set_mode_transition_complete_callback(
        complete_mode_transition,
        NULL
    ) == ESP_OK);
    assert(wifi_manager_stop() == ESP_OK);
    assert(trace_count == 6U);
    assert(trace[0] == TRACE_PREPARE);
    assert(trace[1] == TRACE_MDNS_STOP);
    assert(trace[2] == TRACE_HTTPS_STOP);
    assert(trace[3] == TRACE_STA_RESET);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[5] == TRACE_COMPLETE);
    assert(complete_result == ESP_OK);
    assert(!fake_mdns_ready);
    assert(!fake_https_running);
    assert(!fake_running);
    assert(mode_transition_complete_calls == 1U);
    assert(!mode_transition_running);
    assert(!mode_transition_setup_mode);
    assert(mode_transition_result == ESP_OK);
    assert(mode_transition_cleanup_result == ESP_OK);
    assert(!fake_radio_state_uncertain);

    fake_running = true;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_FAIL;
    reset_trace();
    assert(wifi_manager_stop() == ESP_FAIL);
    assert(trace_count == 1U);
    assert(trace[0] == TRACE_PREPARE);
    assert(fake_mdns_ready);
    assert(fake_https_running);
    assert(fake_running);

    fake_running = true;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_FAIL;
    complete_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_stop() == ESP_FAIL);
    assert(trace_count == 6U);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[5] == TRACE_COMPLETE);
    assert(complete_result == ESP_FAIL);
    assert(fake_running);

    fake_running = true;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_ERR_WIFI_NOT_STARTED;
    complete_result = ESP_FAIL;
    reset_trace();
    assert(wifi_manager_stop() == ESP_OK);
    assert(trace_count == 6U);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[5] == TRACE_COMPLETE);
    assert(complete_result == ESP_OK);
    assert(!fake_running);

    /* STA -> AP target start failure must leave a stopped manager rather than
     * claiming the old STA or a failed AP is still running. */
    fake_running = true;
    fake_setup_mode = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_setup_start_result = ESP_FAIL;
    fake_sta_start_result = ESP_OK;
    mode_transition_complete_calls = 0U;
    mode_transition_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_enter_setup_mode() == ESP_FAIL);
    assert(trace_count == 8U);
    assert(trace[6] == TRACE_STA_RESET);
    assert(trace[7] == TRACE_WIFI_STOP);
    assert(!fake_running);
    assert(!fake_setup_mode);
    assert(!fake_mdns_ready);
    assert(!fake_https_running);
    assert(mode_transition_complete_calls == 1U);
    assert(!mode_transition_running);
    assert(!mode_transition_setup_mode);
    assert(mode_transition_result == ESP_FAIL);
    assert(mode_transition_cleanup_result == ESP_OK);
    assert(!fake_radio_state_uncertain);

    /* AP -> STA target start failure follows the same disconnected fallback. */
    fake_running = true;
    fake_setup_mode = true;
    fake_mdns_ready = false;
    fake_https_running = false;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_setup_start_result = ESP_OK;
    fake_sta_start_result = ESP_FAIL;
    mode_transition_complete_calls = 0U;
    mode_transition_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_FAIL);
    assert(trace_count == 8U);
    assert(trace[6] == TRACE_STA_RESET);
    assert(trace[7] == TRACE_WIFI_STOP);
    assert(!fake_running);
    assert(!fake_setup_mode);
    assert(mode_transition_complete_calls == 1U);
    assert(!mode_transition_running);
    assert(!mode_transition_setup_mode);
    assert(mode_transition_result == ESP_FAIL);
    assert(mode_transition_cleanup_result == ESP_OK);
    assert(!fake_radio_state_uncertain);

    /* If the cleanup stop fails too, keep the disconnected flags but expose
     * that the physical radio state is uncertain and preserve both errors. */
    fake_running = true;
    fake_setup_mode = false;
    fake_radio_state_uncertain = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_cleanup_wifi_stop_result = ESP_ERR_INVALID_STATE;
    fake_setup_start_result = ESP_FAIL;
    fake_sta_start_result = ESP_OK;
    mode_transition_complete_calls = 0U;
    mode_transition_result = ESP_OK;
    mode_transition_cleanup_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_enter_setup_mode() == ESP_FAIL);
    assert(trace_count == 8U);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[7] == TRACE_WIFI_STOP);
    assert(!fake_running);
    assert(!fake_setup_mode);
    assert(fake_radio_state_uncertain);
    assert(mode_transition_complete_calls == 1U);
    assert(!mode_transition_running);
    assert(!mode_transition_setup_mode);
    assert(mode_transition_result == ESP_FAIL);
    assert(mode_transition_cleanup_result == ESP_ERR_INVALID_STATE);

    /* An explicit STOP retries the uncertain cleanup before allowing a new
     * mode transition. */
    fake_wifi_stop_result = ESP_OK;
    fake_cleanup_wifi_stop_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_stop() == ESP_OK);
    assert(trace_count == 2U);
    assert(trace[0] == TRACE_STA_RESET);
    assert(trace[1] == TRACE_WIFI_STOP);
    assert(!fake_radio_state_uncertain);
    assert(mode_transition_complete_calls == 2U);
    assert(mode_transition_cleanup_result == ESP_OK);

    assert(wifi_manager_set_radio_stop_lifecycle_callbacks(
        NULL,
        NULL,
        (void *)1
    ) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_set_radio_stop_lifecycle_callbacks(
        NULL,
        complete_radio_stop,
        NULL
    ) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_set_radio_stop_lifecycle_callbacks(NULL, NULL, NULL) == ESP_OK);
    assert(wifi_manager_set_mode_transition_complete_callback(NULL, (void *)1) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_set_mode_transition_complete_callback(NULL, NULL) == ESP_OK);

    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_setup_start_result = ESP_OK;
    fake_sta_start_result = ESP_OK;
    fake_running = true;
    fake_mdns_ready = false;
    fake_https_running = false;
    reset_trace();
    assert(wifi_manager_stop() == ESP_OK);
    assert(trace_count == 2U);
    assert(trace[0] == TRACE_STA_RESET);
    assert(trace[1] == TRACE_WIFI_STOP);

    puts("wifi manager mode transition tests passed");
    return 0;
}
