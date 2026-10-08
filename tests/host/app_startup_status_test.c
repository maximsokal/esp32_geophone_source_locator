#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_startup_status.h"
#include "esp_err.h"
#include "esp_log.h"
#include "hw_inventory.h"
#include "seismic_acquisition.h"


#define STARTUP_STATUS_LOG_CAPACITY 64U
#define STARTUP_STATUS_LOG_TEXT_CAPACITY 192U

typedef struct
{
    char level;
    char tag[16];
    char text[STARTUP_STATUS_LOG_TEXT_CAPACITY];

} startup_status_log_t;

static startup_status_log_t s_logs[STARTUP_STATUS_LOG_CAPACITY];
static size_t s_log_count;
static bool s_acquisition_running;
static esp_err_t s_acquisition_error;
static bool s_wifi_setup_mode;
static bool s_inventory_available;
static hw_module_state_t s_inventory_state;


void startup_status_test_log(
    char level,
    const char *tag,
    const char *format,
    ...
)
{
    assert(s_log_count < STARTUP_STATUS_LOG_CAPACITY);
    startup_status_log_t *log = &s_logs[s_log_count++];
    log->level = level;
    (void)snprintf(log->tag, sizeof(log->tag), "%s", tag);

    va_list args;
    va_start(args, format);
    (void)vsnprintf(log->text, sizeof(log->text), format, args);
    va_end(args);
}


bool seismic_acquisition_is_running(void)
{
    return s_acquisition_running;
}


esp_err_t seismic_acquisition_last_error(void)
{
    return s_acquisition_error;
}


const char *seismic_acquisition_source_name(void)
{
    return "AD7606";
}


bool wifi_manager_is_setup_mode(void)
{
    return s_wifi_setup_mode;
}


bool hw_inventory_get(hw_module_id_t module, hw_module_info_t *info)
{
    (void)module;
    if (!s_inventory_available || info == NULL) {
        return false;
    }

    info->state = s_inventory_state;
    return true;
}


const char *hw_inventory_state_name(hw_module_state_t state)
{
    return state == HW_STATE_PRESENT ? "PRESENT" : "UNKNOWN";
}


const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_UNKNOWN";
    }
}


static void reset_test_state(void)
{
    memset(s_logs, 0, sizeof(s_logs));
    s_log_count = 0U;
    s_acquisition_running = false;
    s_acquisition_error = ESP_OK;
    s_wifi_setup_mode = false;
    s_inventory_available = true;
    s_inventory_state = HW_STATE_PRESENT;
}


static size_t find_log(char level, const char *text)
{
    size_t found = STARTUP_STATUS_LOG_CAPACITY;
    for (size_t index = 0U; index < s_log_count; ++index) {
        if (s_logs[index].level == level &&
            strcmp(s_logs[index].text, text) == 0) {
            assert(found == STARTUP_STATUS_LOG_CAPACITY);
            assert(strcmp(s_logs[index].tag, "APP") == 0);
            found = index;
        }
    }
    return found;
}


static void assert_log(char level, const char *text)
{
    assert(find_log(level, text) != STARTUP_STATUS_LOG_CAPACITY);
}


static void assert_no_unavailable_log(void)
{
    for (size_t index = 0U; index < s_log_count; ++index) {
        assert(strstr(s_logs[index].text, "UNAVAILABLE") == NULL);
    }
}


static void assert_no_ad7606_warning(void)
{
    for (size_t index = 0U; index < s_log_count; ++index) {
        assert(!(s_logs[index].level == 'W' &&
                 strstr(s_logs[index].text, "AD7606") != NULL));
    }
}


static void test_running_line_is_unchanged(void)
{
    reset_test_state();
    s_acquisition_running = true;
    s_acquisition_error = ESP_FAIL;

    app_startup_log_success();

    assert_log('I', "Seismic acquisition: AD7606 RUNNING");
    assert_no_ad7606_warning();
    assert_no_unavailable_log();
}


static void test_not_started_is_informational(void)
{
    reset_test_state();

    app_startup_log_success();

    assert_log(
        'I',
        "Seismic acquisition: AD7606 NOT STARTED (starts when Measurements is opened)"
    );
    assert_no_ad7606_warning();
    assert_no_unavailable_log();
}


static void assert_failure_line(esp_err_t error, const char *error_name)
{
    reset_test_state();
    s_acquisition_error = error;

    app_startup_log_success();

    char expected[STARTUP_STATUS_LOG_TEXT_CAPACITY];
    (void)snprintf(
        expected,
        sizeof(expected),
        "Seismic acquisition: AD7606 FAILED: %s",
        error_name
    );
    assert_log('W', expected);
    assert_no_unavailable_log();
}


int main(void)
{
    test_running_line_is_unchanged();
    test_not_started_is_informational();
    assert_failure_line(ESP_FAIL, "ESP_FAIL");
    assert_failure_line(ESP_ERR_TIMEOUT, "ESP_ERR_TIMEOUT");
    return 0;
}
