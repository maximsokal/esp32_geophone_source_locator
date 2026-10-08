#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "hw_inventory_reason.h"

typedef struct {
    const char *reason;
    const char *code;
} reason_case_t;

static const reason_case_t s_known_reasons[] = {
    { "ready", "ready" },
    { "no response", "no_response" },
    { "no filesystem", "no_filesystem" },
    { "mount failed", "mount_failed" },
    { "read probe failed", "read_probe_failed" },
    { "topology probe ok", "topology_probe_ok" },
    { "topology probe fault", "topology_probe_fault" },
    { "no upstream response", "no_upstream_response" },
    { "sensor invalid", "sensor_invalid" },
    { "first sample ok", "first_sample_ok" },
    { "no valid sample", "no_valid_sample" },
    { "busy responds", "busy_responds" },
    { "no BUSY response", "no_busy_response" },
    { "presence probe failed", "presence_probe_failed" },
    { "blocked: 5V rail low", "blocked_5v_rail_low" },
    { "init failed", "init_failed" },
    { "start failed", "start_failed" },
    { "sensor ok", "sensor_ok" },
    { "upstream target assigned", "upstream_target_assigned" },
    { "upstream topology fault", "upstream_topology_fault" },
};

static void assert_valid_code(const char *code)
{
    assert(code != NULL);
    assert(code[0] != '\0');
    for (size_t i = 0U; code[i] != '\0'; ++i) {
        const char character = code[i];
        assert((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_');
    }
}

static void test_known_reason_strings(void)
{
    for (size_t i = 0U; i < sizeof(s_known_reasons) / sizeof(s_known_reasons[0]); ++i) {
        const char *code = hw_inventory_reason_code(HW_MODULE_SD_CARD, s_known_reasons[i].reason);
        assert_valid_code(code);
        assert(strcmp(code, s_known_reasons[i].code) == 0);
    }
}

static void test_start_failure_prefix(void)
{
    static const char *const reasons[] = {
        "start failed: ESP_ERR_TIMEOUT",
        "start failed: ESP_ERR_INVALID_STATE",
        "start failed: vendor detail",
    };

    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        const char *code = hw_inventory_reason_code(HW_MODULE_AD7606, reasons[i]);
        assert_valid_code(code);
        assert(strcmp(code, "start_failed") == 0);
    }
}

static void test_none_and_unrecognized_fallbacks(void)
{
    const char *code = hw_inventory_reason_code(HW_MODULE_SD_CARD, NULL);
    assert_valid_code(code);
    assert(strcmp(code, "none") == 0);

    code = hw_inventory_reason_code(HW_MODULE_SD_CARD, "");
    assert_valid_code(code);
    assert(strcmp(code, "none") == 0);

    code = hw_inventory_reason_code(HW_MODULE_SD_CARD, "totally new text");
    assert_valid_code(code);
    assert(strcmp(code, "unrecognized") == 0);

    code = hw_inventory_reason_code(HW_MODULE_SD_CARD, "READY");
    assert_valid_code(code);
    assert(strcmp(code, "unrecognized") == 0);
}

static void test_expected_table_has_unique_keys_and_codes(void)
{
    const size_t count = sizeof(s_known_reasons) / sizeof(s_known_reasons[0]);
    for (size_t i = 0U; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            assert(strcmp(s_known_reasons[i].reason, s_known_reasons[j].reason) != 0);
            assert(strcmp(s_known_reasons[i].code, s_known_reasons[j].code) != 0);
        }
    }
}

int main(void)
{
    test_known_reason_strings();
    test_start_failure_prefix();
    test_none_and_unrecognized_fallbacks();
    test_expected_table_has_unique_keys_and_codes();
    return 0;
}
