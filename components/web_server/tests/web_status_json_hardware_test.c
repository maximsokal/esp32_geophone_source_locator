#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "hw_inventory.h"
#include "hw_inventory_status_fake.h"
#include "web_status_json_hardware.h"

static void test_snapshot_is_serialized_and_escaped(void)
{
    const hw_module_info_t fixture[HW_MODULE_COUNT] = {
        { HW_MODULE_SD_CARD, HW_STATE_ABSENT, HW_REQ_REQUIRED, "missing \"card\"\\retry\n" },
        { HW_MODULE_AD7606, HW_STATE_FAULT, HW_REQ_REQUIRED, "spi timeout" },
        { HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_MAIN_XIAO, HW_STATE_DEGRADED, HW_REQ_OPTIONAL, "limited" },
        { HW_MODULE_BAROMETER, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_POWER_MONITOR, HW_STATE_ABSENT, HW_REQ_OPTIONAL, "not fitted" },
    };
    hw_inventory_status_fake_set(fixture, HW_MODULE_COUNT);

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_hardware(root));
    char *encoded = cJSON_PrintUnformatted(root);
    assert(encoded != NULL);
    assert(strstr(encoded, "\\\"card\\\"") != NULL);
    assert(strstr(encoded, "\\\\retry\\n") != NULL);

    cJSON *parsed = cJSON_Parse(encoded);
    assert(parsed != NULL);
    cJSON *hardware = cJSON_GetObjectItemCaseSensitive(parsed, "hardware");
    assert(cJSON_IsObject(hardware));
    cJSON *modules = cJSON_GetObjectItemCaseSensitive(hardware, "modules");
    assert(cJSON_IsArray(modules));
    assert(cJSON_GetArraySize(modules) == HW_MODULE_COUNT);
    cJSON *first = cJSON_GetArrayItem(modules, 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(first, "id")->valuestring, "sd_card") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(first, "state")->valuestring, "absent") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(first, "required")));
    assert(strcmp(
        cJSON_GetObjectItemCaseSensitive(first, "reason")->valuestring,
        fixture[0].reason
    ) == 0);
    cJSON *optional = cJSON_GetArrayItem(modules, 2);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(optional, "id")->valuestring, "sensor_mux") == 0);
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(optional, "required")));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(optional, "required")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(hardware, "required_missing")));

    cJSON_Delete(parsed);
    cJSON_free(encoded);
    cJSON_Delete(root);
}

static void test_unknown_required_module_is_not_missing(void)
{
    const hw_module_info_t fixture[HW_MODULE_COUNT] = {
        { HW_MODULE_SD_CARD, HW_STATE_PRESENT, HW_REQ_REQUIRED, "" },
        { HW_MODULE_AD7606, HW_STATE_UNKNOWN, HW_REQ_REQUIRED, "" },
        { HW_MODULE_SENSOR_MUX, HW_STATE_ABSENT, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_MAIN_XIAO, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_BAROMETER, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_POWER_MONITOR, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
    };
    hw_inventory_status_fake_set(fixture, HW_MODULE_COUNT);

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_hardware(root));
    cJSON *hardware = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    cJSON *required_missing = cJSON_GetObjectItemCaseSensitive(hardware, "required_missing");
    assert(cJSON_IsBool(required_missing));
    assert(!cJSON_IsTrue(required_missing));
    cJSON_Delete(root);
}

static void test_required_fault_marks_missing(void)
{
    const hw_module_info_t fixture[HW_MODULE_COUNT] = {
        { HW_MODULE_SD_CARD, HW_STATE_PRESENT, HW_REQ_REQUIRED, "" },
        { HW_MODULE_AD7606, HW_STATE_FAULT, HW_REQ_REQUIRED, "timeout" },
        { HW_MODULE_SENSOR_MUX, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_MAIN_XIAO, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_BAROMETER, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
        { HW_MODULE_POWER_MONITOR, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
    };
    hw_inventory_status_fake_set(fixture, HW_MODULE_COUNT);

    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_hardware(root));
    cJSON *hardware = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(hardware, "required_missing")));
    cJSON_Delete(root);
}

int main(void)
{
    test_snapshot_is_serialized_and_escaped();
    test_unknown_required_module_is_not_missing();
    test_required_fault_marks_missing();
    return 0;
}
