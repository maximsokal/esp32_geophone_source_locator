#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "web_status_cache_sections.h"


static esp_err_t s_objects_result;
static esp_err_t s_sessions_result;
static unsigned s_objects_calls;
static unsigned s_sessions_calls;


esp_err_t web_status_json_add_objects(cJSON *root)
{
    assert(root != NULL);
    ++s_objects_calls;
    return s_objects_result;
}


esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id)
{
    assert(root != NULL);
    assert(object_id != NULL);
    ++s_sessions_calls;
    return s_sessions_result;
}


static void reset_results(void)
{
    s_objects_result = ESP_OK;
    s_sessions_result = ESP_OK;
    s_objects_calls = 0U;
    s_sessions_calls = 0U;
}


static void test_invalid_arguments(void)
{
    reset_results();
    assert(web_status_json_add_cache_sections(NULL, true, NULL) == ESP_ERR_INVALID_ARG);
    assert(s_objects_calls == 0U);
    assert(s_sessions_calls == 0U);
}


static void test_object_cache_unavailable_propagates(void)
{
    reset_results();
    cJSON root = {0};
    s_objects_result = ESP_ERR_INVALID_STATE;

    assert(
        web_status_json_add_cache_sections(&root, true, "OBJ_0001") ==
        ESP_ERR_INVALID_STATE
    );
    assert(s_objects_calls == 1U);
    assert(s_sessions_calls == 0U);
}


static void test_session_cache_unavailable_propagates(void)
{
    reset_results();
    cJSON root = {0};
    s_sessions_result = ESP_ERR_INVALID_STATE;

    assert(
        web_status_json_add_cache_sections(&root, false, "OBJ_0001") ==
        ESP_ERR_INVALID_STATE
    );
    assert(s_objects_calls == 0U);
    assert(s_sessions_calls == 1U);
}


static void test_true_json_allocation_failure_stays_no_mem(void)
{
    reset_results();
    cJSON root = {0};
    s_objects_result = ESP_ERR_NO_MEM;

    assert(
        web_status_json_add_cache_sections(&root, true, NULL) ==
        ESP_ERR_NO_MEM
    );
    assert(s_objects_calls == 1U);
    assert(s_sessions_calls == 0U);
}


static void test_success_and_optional_sections(void)
{
    reset_results();
    cJSON root = {0};

    assert(web_status_json_add_cache_sections(&root, false, NULL) == ESP_OK);
    assert(s_objects_calls == 0U);
    assert(s_sessions_calls == 0U);

    assert(
        web_status_json_add_cache_sections(&root, true, "OBJ_0001") ==
        ESP_OK
    );
    assert(s_objects_calls == 1U);
    assert(s_sessions_calls == 1U);
}


int main(void)
{
    test_invalid_arguments();
    test_object_cache_unavailable_propagates();
    test_session_cache_unavailable_propagates();
    test_true_json_allocation_failure_stays_no_mem();
    test_success_and_optional_sections();
    return 0;
}
