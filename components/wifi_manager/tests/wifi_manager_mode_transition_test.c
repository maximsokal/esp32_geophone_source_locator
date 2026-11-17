#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app_events.h"
#include "freertos/FreeRTOS.h"
#include "wifi_manager.h"
#include "wifi_manager_internal.h"

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
static bool fake_initialized;
static bool fake_running;
static bool fake_setup_mode;
static bool fake_radio_state_uncertain;
static bool fake_transition_active;
static bool fake_network_events_suppressed;
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
    /* The serialized transaction is still held while completion reconciles
     * consumers, but raw-event suppression must already reflect final state. */
    assert(fake_transition_active);
    assert(!fake_network_events_suppressed);
    ++mode_transition_complete_calls;
    mode_transition_running = running;
    mode_transition_setup_mode = setup_mode;
    mode_transition_result = transition_result;
    mode_transition_cleanup_result = cleanup_result;
}

void wifi_manager_internal_get_state(
    wifi_manager_internal_state_t *state
)
{
    assert(state != NULL);
    *state = (wifi_manager_internal_state_t) {
        .initialized = fake_initialized,
        .running = fake_running,
        .setup_mode = fake_setup_mode,
        .radio_state_uncertain = fake_radio_state_uncertain,
        .transition_active = fake_transition_active,
        .network_events_suppressed = fake_network_events_suppressed,
    };
}

bool wifi_manager_internal_initialized(void)
{
    return fake_initialized;
}

esp_err_t wifi_manager_internal_begin_transition(void)
{
    if (fake_transition_active) {
        return ESP_ERR_INVALID_STATE;
    }

    fake_transition_active = true;
    fake_network_events_suppressed = true;
    return ESP_OK;
}

void wifi_manager_internal_end_transition(void)
{
    assert(fake_transition_active);
    fake_transition_active = false;
    fake_network_events_suppressed = false;
}

void wifi_manager_internal_set_initialized(bool initialized)
{
    fake_initialized = initialized;
}

void wifi_manager_internal_commit_state(
    bool running,
    bool setup_mode,
    bool radio_state_uncertain
)
{
    fake_running = running;
    fake_setup_mode = running && setup_mode;
    fake_radio_state_uncertain = radio_state_uncertain;
}

void wifi_manager_internal_commit_final_state(
    bool running,
    bool setup_mode,
    bool radio_state_uncertain
)
{
    wifi_manager_internal_commit_state(
        running,
        setup_mode,
        radio_state_uncertain
    );
    fake_network_events_suppressed = false;
}

bool wifi_manager_is_running(void)
{
    return fake_running;
}

bool wifi_manager_internal_setup_mode(void)
{
    return fake_running && fake_setup_mode;
}

void wifi_manager_internal_set_radio_state_uncertain(bool uncertain)
{
    fake_radio_state_uncertain = uncertain;
}

bool wifi_manager_internal_radio_state_uncertain(void)
{
    return fake_radio_state_uncertain;
}

esp_err_t wifi_manager_internal_start_setup_mode(void)
{
    return fake_setup_start_result;
}

void wifi_manager_runtime_clear_setup_client(void)
{
}

/* The clock and the station's address, as the production code reads them. */
static int64_t fake_now_us;
static bool fake_sta_has_ip;

int64_t esp_timer_get_time(void)
{
    return fake_now_us;
}

esp_err_t wifi_manager_sta_ip_string(char *text, size_t text_size)
{
    if (text == NULL || text_size < 16U) return ESP_ERR_INVALID_ARG;
    if (!fake_sta_has_ip) return ESP_ERR_NOT_FOUND;
    text[0] = '1'; text[1] = '\0';
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t wifi_manager_sta_start(void)
{
    return fake_sta_start_result;
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

esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    (void)type;
    (void)value;
    return ESP_OK;
}

int main(void)
{
    fake_initialized = true;
    fake_running = true;
    fake_setup_mode = false;
    fake_radio_state_uncertain = false;
    fake_transition_active = false;
    fake_network_events_suppressed = false;
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

    /* A second caller from another task must not overlap an active radio
     * transaction. No radio or lifecycle callback may run on rejection. */
    fake_transition_active = true;
    assert(wifi_manager_stop() == ESP_ERR_INVALID_STATE);
    assert(wifi_manager_enter_setup_mode() == ESP_ERR_INVALID_STATE);
    assert(wifi_manager_connect_saved() == ESP_ERR_INVALID_STATE);
    assert(trace_count == 0U);
    fake_transition_active = false;

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
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
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
    assert(!fake_setup_mode);
    assert(mode_transition_complete_calls == 1U);
    assert(!mode_transition_running);
    assert(!mode_transition_setup_mode);
    assert(mode_transition_result == ESP_OK);
    assert(mode_transition_cleanup_result == ESP_OK);
    assert(!fake_radio_state_uncertain);

    fake_running = true;
    fake_setup_mode = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_FAIL;
    reset_trace();
    assert(wifi_manager_stop() == ESP_FAIL);
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
    assert(trace_count == 1U);
    assert(trace[0] == TRACE_PREPARE);
    assert(fake_mdns_ready);
    assert(fake_https_running);
    assert(fake_running);

    fake_running = true;
    fake_setup_mode = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_FAIL;
    complete_result = ESP_OK;
    reset_trace();
    assert(wifi_manager_stop() == ESP_FAIL);
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
    assert(trace_count == 6U);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[5] == TRACE_COMPLETE);
    assert(complete_result == ESP_FAIL);
    assert(fake_running);

    fake_running = true;
    fake_setup_mode = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_ERR_WIFI_NOT_STARTED;
    complete_result = ESP_FAIL;
    reset_trace();
    assert(wifi_manager_stop() == ESP_OK);
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
    assert(trace_count == 6U);
    assert(trace[4] == TRACE_WIFI_STOP);
    assert(trace[5] == TRACE_COMPLETE);
    assert(complete_result == ESP_OK);
    assert(!fake_running);
    assert(!fake_setup_mode);

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
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
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

    /* Successful STA -> AP commits running/setup together after AP start. */
    fake_running = true;
    fake_setup_mode = false;
    fake_mdns_ready = true;
    fake_https_running = true;
    prepare_result = ESP_OK;
    fake_wifi_stop_result = ESP_OK;
    fake_setup_start_result = ESP_OK;
    mode_transition_complete_calls = 0U;
    reset_trace();
    assert(wifi_manager_enter_setup_mode() == ESP_OK);
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
    assert(fake_running);
    assert(fake_setup_mode);
    assert(!fake_radio_state_uncertain);
    assert(mode_transition_complete_calls == 1U);
    assert(mode_transition_running);
    assert(mode_transition_setup_mode);

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
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
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
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
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
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
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
    fake_setup_mode = false;
    fake_mdns_ready = false;
    fake_https_running = false;
    reset_trace();
    assert(wifi_manager_stop() == ESP_OK);
    assert(!fake_transition_active);
    assert(!fake_network_events_suppressed);
    assert(trace_count == 2U);
    assert(trace[0] == TRACE_STA_RESET);
    assert(trace[1] == TRACE_WIFI_STOP);

    /*
     * Board log 2026-11-17 10:29: the operator pressed OK on "connect to the saved network" at 176.62 s; the station was
     * associated at 179.23 s, and the second OK press (179.25 s, handled 179.48 s) called wifi_manager_connect_saved()
     * again, which stopped the radio ("run -> init") 0.25 s after the link had come up. The same press at 9.8 s tore down a
     * link that had been up for six seconds. A connect request must not undo a connection that is up or that was started a
     * moment ago, and it must still work as the retry when the last attempt has gone nowhere.
     */
    fake_initialized = true;
    fake_radio_state_uncertain = false;
    fake_transition_active = false;
    fake_network_events_suppressed = false;
    fake_wifi_stop_result = ESP_OK;
    fake_cleanup_wifi_stop_result = ESP_OK;
    fake_sta_start_result = ESP_OK;
    prepare_result = ESP_OK;
    fake_sta_has_ip = false;
    fake_now_us = 1000000;

    /* 1. From the setup access point: always goes through (this is the "I have just entered the network" case). */
    fake_running = true;
    fake_setup_mode = true;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 1U);
    assert(fake_running && !fake_setup_mode);

    /* 2. The same request 2.6 s later, while the first attempt is still associating: nothing happens. */
    fake_now_us += 2600000;
    reset_trace();
    mode_transition_complete_calls = 0U;
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 0U);          /* the radio was not stopped */
    assert(trace_count == 0U);                   /* no lifecycle callback ran: HTTPS and mDNS stay as they are */
    assert(mode_transition_complete_calls == 0U);
    assert(fake_running && !fake_setup_mode);
    assert(!fake_transition_active && !fake_network_events_suppressed);

    /* 3. The link has come up (an address was obtained): a request, however late, leaves it alone. */
    fake_sta_has_ip = true;
    fake_now_us += 60000000;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 0U && trace_count == 0U);

    /* 4. No address and the settle time is over: the attempt went nowhere, the request is the retry and restarts it. */
    fake_sta_has_ip = false;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 1U);
    fake_now_us += 7900000;                      /* 7.9 s after that restart: still inside the settle time */
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 0U && trace_count == 0U);
    fake_now_us += 200000;                       /* 8.1 s: over */
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 1U);

    /* 5. A failed start does not start the settle time: the next request may try again at once. */
    fake_running = true;
    fake_setup_mode = true;
    fake_sta_start_result = ESP_FAIL;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_FAIL);
    fake_sta_start_result = ESP_OK;
    fake_now_us += 100000;
    fake_running = true;
    fake_setup_mode = true;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 1U);

    /* 6. After the radio was stopped or the setup access point started, a request goes through again. */
    fake_now_us += 100000;
    fake_running = true;
    fake_setup_mode = false;
    reset_trace();
    assert(wifi_manager_enter_setup_mode() == ESP_OK);
    fake_running = false;
    fake_setup_mode = false;
    fake_now_us += 100000;
    reset_trace();
    assert(wifi_manager_connect_saved() == ESP_OK);
    assert(fake_wifi_stop_calls == 1U);

    puts("wifi manager mode transition tests passed");
    return 0;
}
