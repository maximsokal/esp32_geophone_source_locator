#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "ad7606_power_policy.h"
#include "ad7606_serial.h"
#include "hw_inventory.h"
#include "seismic_source_driver.h"
#include "seismic_source_ad7606_inventory_policy.h"


static esp_err_t s_acquisition_start_result = ESP_OK;
static esp_err_t s_presence_start_result = ESP_OK;
static bool s_acquisition_gate_refused;
static ad7606_serial_observation_t s_last_observation;
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
    if (s_acquisition_gate_refused) {
        status->power_gate = AD7606_POWER_GATE_RAIL_LOW;
        status->power_fault_latched = true;
    }
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


esp_err_t ad7606_driver_get_last_serial_observation(
    ad7606_serial_observation_t *observation
)
{
    if (observation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *observation = s_last_observation;
    return ESP_OK;
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


static void assert_reason_is_ascii_and_fits(const char *reason)
{
    const size_t length = strlen(reason);
    assert(length <= HW_INVENTORY_REASON_LEN - 1U);
    for (size_t index = 0U; index < length; ++index) {
        assert((unsigned char)reason[index] <= 0x7FU);
    }
}


static void test_probe_result_is_reported(
    esp_err_t probe_result,
    hw_module_state_t expected_state,
    const char *expected_reason
)
{
    hw_inventory_init();
    s_acquisition_start_result = ESP_OK;
    s_acquisition_gate_refused = false;
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
    s_acquisition_gate_refused = true;
    const unsigned presence_calls_before = s_presence_start_calls;

    assert(g_seismic_source_ad7606.start() == ESP_ERR_INVALID_STATE);
    assert(s_presence_start_calls == presence_calls_before);

    const hw_module_info_t module = get_ad7606_inventory();
    assert(module.state == HW_STATE_UNKNOWN);
    /* The gate refusal is now recorded with its reason while state stays UNKNOWN. */
    assert(strcmp(module.reason, "blocked: 5V rail low") == 0);
    assert_reason_is_ascii_and_fits(module.reason);
}


static void test_acquisition_start_failure_is_reported(
    esp_err_t start_result,
    bool gate_refused,
    bool conversion_complete,
    bool spi_transaction_ok,
    hw_module_state_t expected_state,
    const char *expected_reason
)
{
    hw_inventory_init();
    s_acquisition_start_result = start_result;
    s_acquisition_gate_refused = gate_refused;
    s_last_observation = (ad7606_serial_observation_t){
        .conversion_complete = conversion_complete,
        .spi_transaction_ok = spi_transaction_ok,
    };

    const unsigned acquisition_starts_before = s_acquisition_start_calls;
    const unsigned presence_starts_before = s_presence_start_calls;
    const unsigned acquisition_stops_before = s_acquisition_stop_calls;
    assert(g_seismic_source_ad7606.start() == start_result);
    assert(s_acquisition_start_calls == acquisition_starts_before + 1U);
    assert(s_presence_start_calls == presence_starts_before);
    assert(s_acquisition_stop_calls == acquisition_stops_before);

    const hw_module_info_t module = get_ad7606_inventory();
    assert(module.state == expected_state);
    assert(module.requirement == HW_REQ_REQUIRED);
    assert(strcmp(module.reason, expected_reason) == 0);
    assert_reason_is_ascii_and_fits(module.reason);
}


static void test_start_failure_policy_mapping(void)
{
    const struct {
        esp_err_t error;
        seismic_source_ad7606_start_failure_stage_t stage;
        hw_module_state_t expected_state;
        const char *expected_reason;
    } cases[] = {
        {
            ESP_ERR_INVALID_STATE,
            SEISMIC_SOURCE_AD7606_START_FAILURE_RAIL_GATE,
            HW_STATE_UNKNOWN,
            "blocked: 5V rail low",
        },
        {
            ESP_ERR_INVALID_STATE,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_UNKNOWN,
            "start failed: ESP_ERR_INVALID_STATE",
        },
        {
            ESP_ERR_TIMEOUT,
            SEISMIC_SOURCE_AD7606_START_FAILURE_BUSY_TIMEOUT,
            HW_STATE_ABSENT,
            "no BUSY response",
        },
        {
            ESP_ERR_TIMEOUT,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_UNKNOWN,
            "start failed: ESP_ERR_TIMEOUT",
        },
        {
            ESP_ERR_NOT_FOUND,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_UNKNOWN,
            "start failed: ESP_ERR_NOT_FOUND",
        },
        {
            ESP_ERR_NO_MEM,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_FAULT,
            "init failed",
        },
        {
            ESP_FAIL,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_FAULT,
            "init failed",
        },
        {
            ESP_ERR_INVALID_ARG,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_UNKNOWN,
            "start failed: ESP_ERR_INVALID_ARG",
        },
        {
            0x777,
            SEISMIC_SOURCE_AD7606_START_FAILURE_UNRESOLVED,
            HW_STATE_FAULT,
            "init failed",
        },
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const seismic_source_ad7606_start_failure_t result =
            seismic_source_ad7606_classify_start_failure(
                cases[index].error,
                cases[index].stage
            );
        assert(result.state == cases[index].expected_state);
        assert(strcmp(result.reason, cases[index].expected_reason) == 0);
        assert_reason_is_ascii_and_fits(result.reason);
    }
}


static void test_start_failure_results_reach_inventory(void)
{
    test_acquisition_start_failure_is_reported(
        ESP_ERR_INVALID_STATE,
        false,
        false,
        false,
        HW_STATE_UNKNOWN,
        "start failed: ESP_ERR_INVALID_STATE"
    );
    test_acquisition_start_failure_is_reported(
        ESP_ERR_TIMEOUT,
        false,
        false,
        false,
        HW_STATE_ABSENT,
        "no BUSY response"
    );
    test_acquisition_start_failure_is_reported(
        ESP_ERR_TIMEOUT,
        false,
        true,
        true,
        HW_STATE_ABSENT,
        "no BUSY response"
    );
    test_acquisition_start_failure_is_reported(
        ESP_ERR_TIMEOUT,
        false,
        true,
        false,
        HW_STATE_UNKNOWN,
        "start failed: ESP_ERR_TIMEOUT"
    );
    /* PCNT setup also returns NOT_FOUND when counter resources are exhausted. */
    test_acquisition_start_failure_is_reported(
        ESP_ERR_NOT_FOUND,
        false,
        false,
        false,
        HW_STATE_UNKNOWN,
        "start failed: ESP_ERR_NOT_FOUND"
    );
    test_acquisition_start_failure_is_reported(
        ESP_ERR_NO_MEM,
        false,
        false,
        false,
        HW_STATE_FAULT,
        "init failed"
    );
    test_acquisition_start_failure_is_reported(
        ESP_FAIL,
        false,
        false,
        false,
        HW_STATE_FAULT,
        "init failed"
    );
    test_acquisition_start_failure_is_reported(
        ESP_ERR_INVALID_ARG,
        false,
        false,
        false,
        HW_STATE_UNKNOWN,
        "start failed: ESP_ERR_INVALID_ARG"
    );
}


int main(void)
{
    test_acquisition_start_failure_does_not_claim_presence();
    test_start_failure_policy_mapping();
    test_start_failure_results_reach_inventory();
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
