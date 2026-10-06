#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ad7606_acquisition.h"
#include "ad7606_power_policy.h"
#include "ad7606_serial.h"
#include "hw_inventory.h"
#include "seismic_source_driver.h"


static esp_err_t s_acquisition_start_result = ESP_OK;
static esp_err_t s_presence_start_result = ESP_OK;
static unsigned s_acquisition_start_calls;
static unsigned s_acquisition_stop_calls;
static unsigned s_presence_start_calls;


esp_err_t ad7606_acquisition_start(void)
{
    ++s_acquisition_start_calls;
    return s_acquisition_start_result;
}


void ad7606_acquisition_stop(void)
{
    ++s_acquisition_stop_calls;
}


esp_err_t ad7606_acquisition_get_status(ad7606_acquisition_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *status = (ad7606_acquisition_status_t){0};
    return ESP_OK;
}


esp_err_t ad7606_driver_presence_monitor_start(void)
{
    ++s_presence_start_calls;
    return s_presence_start_result;
}


void ad7606_driver_presence_monitor_stop(void)
{
}


bool ad7606_driver_presence_is_healthy(void)
{
    return false;
}


const char *ad7606_serial_pattern_name(ad7606_serial_pattern_t pattern)
{
    (void)pattern;
    return "test";
}


const char *ad7606_serial_quality_name(ad7606_serial_quality_t quality)
{
    (void)quality;
    return "test";
}


const char *ad7606_signal_quality_name(ad7606_signal_quality_t quality)
{
    (void)quality;
    return "test";
}


const char *ad7606_serial_stream_state_name(ad7606_serial_stream_state_t state)
{
    (void)state;
    return "test";
}


const char *ad7606_power_gate_name(ad7606_power_gate_t gate)
{
    (void)gate;
    return "test";
}


static hw_module_info_t get_ad7606_inventory(void)
{
    hw_module_info_t module;
    assert(hw_inventory_get(HW_MODULE_AD7606, &module));
    return module;
}


static void test_probe_result_is_reported(
    esp_err_t probe_result,
    hw_module_state_t expected_state,
    const char *expected_reason
)
{
    hw_inventory_init();
    s_acquisition_start_result = ESP_OK;
    s_presence_start_result = probe_result;

    const unsigned acquisition_starts_before = s_acquisition_start_calls;
    const unsigned presence_starts_before = s_presence_start_calls;
    const unsigned acquisition_stops_before = s_acquisition_stop_calls;
    assert(g_seismic_source_ad7606.start() == probe_result);
    assert(s_acquisition_start_calls == acquisition_starts_before + 1U);
    assert(s_presence_start_calls == presence_starts_before + 1U);

    const hw_module_info_t module = get_ad7606_inventory();
    assert(module.state == expected_state);
    assert(module.requirement == HW_REQ_REQUIRED);
    assert(strcmp(module.reason, expected_reason) == 0);
    assert(
        s_acquisition_stop_calls == acquisition_stops_before +
            (probe_result == ESP_OK ? 0U : 1U)
    );
}


static void test_acquisition_start_failure_does_not_claim_presence(void)
{
    hw_inventory_init();
    s_acquisition_start_result = ESP_ERR_INVALID_STATE;
    const unsigned presence_calls_before = s_presence_start_calls;

    assert(g_seismic_source_ad7606.start() == ESP_ERR_INVALID_STATE);
    assert(s_presence_start_calls == presence_calls_before);

    const hw_module_info_t module = get_ad7606_inventory();
    assert(module.state == HW_STATE_UNKNOWN);
    assert(module.reason[0] == '\0');
}


int main(void)
{
    test_acquisition_start_failure_does_not_claim_presence();
    test_probe_result_is_reported(ESP_OK, HW_STATE_PRESENT, "busy responds");
    test_probe_result_is_reported(
        ESP_ERR_NOT_FOUND,
        HW_STATE_ABSENT,
        "no BUSY response"
    );
    test_probe_result_is_reported(
        ESP_ERR_TIMEOUT,
        HW_STATE_FAULT,
        "presence probe failed"
    );

    puts("AD7606 inventory integration tests passed");
    return 0;
}
