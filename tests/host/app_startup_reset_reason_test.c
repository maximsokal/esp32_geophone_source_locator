#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_startup_reset_reason.h"
#include "app_startup_status.h"
#include "esp_system.h"
#include "hw_inventory.h"
#include "seismic_acquisition.h"

static esp_reset_reason_t s_reset_reason = ESP_RST_PANIC;
static char s_log[2048];
static char s_status_reason[24];
static int s_status_code;
static uint32_t s_status_boot_id;
static uint64_t s_status_uptime_ms;

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "OK" : "ERROR";
}

void max484_test_log(const char *tag, const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) return;
    const size_t offset = strlen(s_log);
    if (offset + (size_t)written + 2U < sizeof(s_log)) {
        (void)snprintf(s_log + offset, sizeof(s_log) - offset, "%s: %s\n", tag, line);
    }
}

esp_reset_reason_t esp_reset_reason(void)
{
    return s_reset_reason;
}

int64_t esp_timer_get_time(void)
{
    return 12345678LL;
}

uint32_t object_recovery_boot_id(void)
{
    return 1156U;
}

void web_status_json_set_reset_diagnostics(
    const char *reason_name,
    int reason_code,
    uint32_t boot_id,
    uint64_t uptime_ms
)
{
    (void)snprintf(s_status_reason, sizeof(s_status_reason), "%s", reason_name);
    s_status_code = reason_code;
    s_status_boot_id = boot_id;
    s_status_uptime_ms = uptime_ms;
}

bool hw_inventory_get(hw_module_id_t module, hw_module_info_t *info)
{
    assert(module == HW_MODULE_SD_CARD && info != NULL);
    info->state = HW_STATE_PRESENT;
    return true;
}

const char *hw_inventory_state_name(hw_state_t state)
{
    return state == HW_STATE_PRESENT ? "PRESENT" : "UNKNOWN";
}

bool wifi_manager_is_setup_mode(void)
{
    return false;
}

bool seismic_acquisition_is_running(void)
{
    return false;
}

esp_err_t seismic_acquisition_last_error(void)
{
    return ESP_OK;
}

const char *seismic_acquisition_source_name(void)
{
    return "AD7606";
}

static void test_reset_reason_mapping(void)
{
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_UNKNOWN), "UNKNOWN") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_POWERON), "POWERON") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_EXT), "EXT") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_SW), "SW") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_PANIC), "PANIC") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_INT_WDT), "INT_WDT") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_TASK_WDT), "TASK_WDT") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_WDT), "WDT") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_DEEPSLEEP), "DEEPSLEEP") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_BROWNOUT), "BROWNOUT") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_SDIO), "SDIO") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_USB), "USB") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_JTAG), "JTAG") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_EFUSE), "EFUSE") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_PWR_GLITCH), "PWR_GLITCH") == 0);
    assert(strcmp(app_startup_reset_reason_name(ESP_RST_CPU_LOCKUP), "CPU_LOCKUP") == 0);
    assert(strcmp(app_startup_reset_reason_name((esp_reset_reason_t)99), "UNKNOWN") == 0);
}

static void test_reset_reason_is_logged_and_available_to_status(void)
{
    app_startup_log_success();
    assert(strstr(s_log, "RESET_REASON reason=PANIC") != NULL);
    assert(strstr(s_log, "uptime_ms=12345") != NULL);
    assert(strstr(s_log, "boot_id=1156") != NULL);
    assert(strcmp(s_status_reason, "PANIC") == 0);
    assert(s_status_code == (int)ESP_RST_PANIC);
    assert(s_status_boot_id == 1156U && s_status_uptime_ms == 12345U);

    const app_startup_reset_diagnostics_t *diagnostics =
        app_startup_get_reset_diagnostics();
    assert(diagnostics != NULL && diagnostics->valid);
    assert(diagnostics->reason == ESP_RST_PANIC);
    assert(strcmp(diagnostics->reason_name, "PANIC") == 0);
    assert(diagnostics->boot_id == 1156U && diagnostics->uptime_ms == 12345U);
}

int main(void)
{
    test_reset_reason_mapping();
    test_reset_reason_is_logged_and_available_to_status();
    return 0;
}
