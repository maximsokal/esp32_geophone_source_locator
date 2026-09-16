#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_netif.h"
#include "freertos/portmacro.h"
#include "wifi_manager.h"
#include "wifi_manager_internal.h"

static esp_netif_t s_ap_netif;

static esp_err_t s_platform_init_result;
static esp_err_t s_sta_init_result;
static esp_err_t s_credentials_result;
static esp_err_t s_sta_start_result;
static esp_err_t s_setup_start_result;
static bool s_has_credentials;

static unsigned s_platform_init_calls;
static unsigned s_sta_init_calls;
static unsigned s_credentials_calls;
static unsigned s_sta_start_calls;
static unsigned s_setup_start_calls;
static unsigned s_critical_enters;
static unsigned s_critical_exits;

void test_port_enter_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(mux->depth == 0U);
    mux->depth = 1U;
    ++s_critical_enters;
}

void test_port_exit_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(mux->depth == 1U);
    mux->depth = 0U;
    ++s_critical_exits;
}

void test_esp_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        default: return "ESP_ERR";
    }
}

static void assert_transition_snapshot(
    bool expected_running,
    bool expected_setup_mode
)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    assert(state.transition_active);
    assert(state.network_events_suppressed);
    assert(state.running == expected_running);
    assert(state.setup_mode == expected_setup_mode);
}

esp_err_t wifi_manager_platform_init(void)
{
    ++s_platform_init_calls;
    assert_transition_snapshot(false, false);
    return s_platform_init_result;
}

esp_netif_t *wifi_manager_platform_ap_netif(void)
{
    return &s_ap_netif;
}

esp_err_t wifi_manager_sta_init(void)
{
    ++s_sta_init_calls;
    assert_transition_snapshot(false, false);
    return s_sta_init_result;
}

esp_err_t wifi_manager_credentials_has_saved(bool *has_credentials)
{
    assert(has_credentials != NULL);
    ++s_credentials_calls;
    assert_transition_snapshot(false, false);
    if (s_credentials_result == ESP_OK) {
        *has_credentials = s_has_credentials;
    }
    return s_credentials_result;
}

esp_err_t wifi_manager_sta_start(void)
{
    ++s_sta_start_calls;
    assert_transition_snapshot(false, false);
    return s_sta_start_result;
}

void wifi_manager_sta_reset_state(void)
{
}

esp_err_t wifi_manager_setup_ap_start(esp_netif_t *ap_netif)
{
    assert(ap_netif == &s_ap_netif);
    ++s_setup_start_calls;
    assert_transition_snapshot(false, false);
    return s_setup_start_result;
}

static void reset_fakes(void)
{
    wifi_manager_internal_end_transition();
    wifi_manager_internal_set_initialized(false);
    wifi_manager_internal_commit_state(false, false, false);

    s_platform_init_result = ESP_OK;
    s_sta_init_result = ESP_OK;
    s_credentials_result = ESP_OK;
    s_sta_start_result = ESP_OK;
    s_setup_start_result = ESP_OK;
    s_has_credentials = true;

    s_platform_init_calls = 0U;
    s_sta_init_calls = 0U;
    s_credentials_calls = 0U;
    s_sta_start_calls = 0U;
    s_setup_start_calls = 0U;
    s_critical_enters = 0U;
    s_critical_exits = 0U;
}

static void assert_lock_balanced(void)
{
    assert(s_critical_enters == s_critical_exits);
}

static void assert_state(
    bool initialized,
    bool running,
    bool setup_mode,
    bool uncertain,
    bool transition_active,
    bool network_events_suppressed
)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    assert(state.initialized == initialized);
    assert(state.running == running);
    assert(state.setup_mode == setup_mode);
    assert(state.radio_state_uncertain == uncertain);
    assert(state.transition_active == transition_active);
    assert(state.network_events_suppressed == network_events_suppressed);
}

static void test_atomic_state_invariants(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);

    wifi_manager_internal_commit_state(false, true, false);
    assert_state(true, false, false, false, false, false);
    assert(!wifi_manager_is_running());
    assert(!wifi_manager_is_setup_mode());

    wifi_manager_internal_commit_state(true, true, false);
    assert_state(true, true, true, false, false, false);
    assert(wifi_manager_is_running());
    assert(wifi_manager_is_setup_mode());

    wifi_manager_internal_commit_state(true, false, true);
    assert_state(true, true, false, true, false, false);
    assert(wifi_manager_is_running());
    assert(!wifi_manager_is_setup_mode());
    assert(wifi_manager_is_radio_state_uncertain());
    assert_lock_balanced();
}

static void test_transition_gate_rejects_overlap(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);

    assert(wifi_manager_internal_begin_transition() == ESP_OK);
    assert(wifi_manager_internal_begin_transition() == ESP_ERR_INVALID_STATE);
    assert_state(true, false, false, false, true, true);

    assert(wifi_manager_start() == ESP_ERR_INVALID_STATE);
    assert(s_credentials_calls == 0U);
    assert(s_sta_start_calls == 0U);
    assert(s_setup_start_calls == 0U);

    wifi_manager_internal_end_transition();
    assert_state(true, false, false, false, false, false);
    assert_lock_balanced();
}

static void test_final_commit_reopens_events_before_transaction_unlock(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);

    assert(wifi_manager_internal_begin_transition() == ESP_OK);
    assert_state(true, false, false, false, true, true);

    wifi_manager_internal_commit_final_state(true, true, false);
    assert_state(true, true, true, false, true, false);

    wifi_manager_internal_end_transition();
    assert_state(true, true, true, false, false, false);
    assert_lock_balanced();
}

static void test_init_failure_releases_transition_and_retries(void)
{
    reset_fakes();

    s_platform_init_result = ESP_FAIL;
    assert(wifi_manager_init() == ESP_FAIL);
    assert_state(false, false, false, false, false, false);
    assert(s_platform_init_calls == 1U);
    assert(s_sta_init_calls == 0U);

    s_platform_init_result = ESP_OK;
    s_sta_init_result = ESP_FAIL;
    assert(wifi_manager_init() == ESP_FAIL);
    assert_state(false, false, false, false, false, false);
    assert(s_platform_init_calls == 2U);
    assert(s_sta_init_calls == 1U);

    s_sta_init_result = ESP_OK;
    assert(wifi_manager_init() == ESP_OK);
    assert_state(true, false, false, false, false, false);
    assert(s_platform_init_calls == 3U);
    assert(s_sta_init_calls == 2U);

    assert(wifi_manager_init() == ESP_OK);
    assert(s_platform_init_calls == 3U);
    assert(s_sta_init_calls == 2U);
    assert_lock_balanced();
}

static void test_start_saved_sta_commits_after_hardware_start(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);
    s_has_credentials = true;

    assert(wifi_manager_start() == ESP_OK);
    assert(s_credentials_calls == 1U);
    assert(s_sta_start_calls == 1U);
    assert(s_setup_start_calls == 0U);
    assert_state(true, true, false, false, false, false);

    assert(wifi_manager_start() == ESP_OK);
    assert(s_credentials_calls == 1U);
    assert(s_sta_start_calls == 1U);
    assert_lock_balanced();
}

static void test_start_setup_ap_commits_running_and_setup_together(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);
    s_has_credentials = false;

    assert(wifi_manager_start() == ESP_OK);
    assert(s_credentials_calls == 1U);
    assert(s_sta_start_calls == 0U);
    assert(s_setup_start_calls == 1U);
    assert_state(true, true, true, false, false, false);
    assert(wifi_manager_is_setup_mode());
    assert_lock_balanced();
}

static void test_start_failures_release_transition(void)
{
    reset_fakes();
    wifi_manager_internal_set_initialized(true);

    s_credentials_result = ESP_FAIL;
    assert(wifi_manager_start() == ESP_FAIL);
    assert_state(true, false, false, false, false, false);

    s_credentials_result = ESP_OK;
    s_has_credentials = true;
    s_sta_start_result = ESP_FAIL;
    assert(wifi_manager_start() == ESP_FAIL);
    assert_state(true, false, false, false, false, false);

    s_sta_start_result = ESP_OK;
    assert(wifi_manager_start() == ESP_OK);
    assert_state(true, true, false, false, false, false);
    assert_lock_balanced();
}

int main(void)
{
    test_atomic_state_invariants();
    test_transition_gate_rejects_overlap();
    test_final_commit_reopens_events_before_transaction_unlock();
    test_init_failure_releases_transition_and_retries();
    test_start_saved_sta_commits_after_hardware_start();
    test_start_setup_ap_commits_running_and_setup_together();
    test_start_failures_release_transition();

    puts("wifi manager state tests passed");
    return 0;
}
