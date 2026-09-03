from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f'Expected fragment not found for {label}:\n{old}')
    return text.replace(old, new, 1)


profile_path = Path('components/object_store/object_store_profile.c')
profile = profile_path.read_text(encoding='utf-8')
profile_original = profile
profile = replace_once(
    profile,
    '#include <stdio.h>\n#include <string.h>',
    '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>',
    'object-store stdlib include',
)
profile = replace_once(
    profile,
    '''    char buffer[OBJECT_STORE_JSON_BUFFER_SIZE];
    err = storage_manager_read_text(path, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
''',
    '''    char *buffer = malloc(OBJECT_STORE_JSON_BUFFER_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = storage_manager_read_text(path, buffer, OBJECT_STORE_JSON_BUFFER_SIZE);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
''',
    'object metadata scratch buffer',
)
profile = replace_once(
    profile,
    '''    char buffer[1024];
    esp_err_t err = storage_manager_read_text(OBJECT_STORE_SETTINGS_PATH, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
''',
    '''    char *buffer = malloc(1024U);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_manager_read_text(OBJECT_STORE_SETTINGS_PATH, buffer, 1024U);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
''',
    'object settings scratch buffer',
)
profile = replace_once(
    profile,
    '''    object_store_summary_t summaries[OBJECT_STORE_MAX_OBJECTS];
    size_t count = 0;
    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS ? max_objects : OBJECT_STORE_MAX_OBJECTS;
    err = object_store_fs_list(summaries, bounded, &count, object_store_internal_active_id());

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            err = object_store_json_read_profile_locked(
                summaries[i].id,
                summaries[i].active,
                &objects[i]
            );
            if (err != ESP_OK) break;
        }
    }

    if (err == ESP_OK) *object_count = count;
    object_store_internal_end_file_operation();
    return err;
''',
    '''    size_t count = 0;
    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS ? max_objects : OBJECT_STORE_MAX_OBJECTS;
    object_store_summary_t *summaries = calloc(bounded, sizeof(*summaries));
    if (summaries == NULL) {
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }

    err = object_store_fs_list(summaries, bounded, &count, object_store_internal_active_id());

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            err = object_store_json_read_profile_locked(
                summaries[i].id,
                summaries[i].active,
                &objects[i]
            );
            if (err != ESP_OK) break;
        }
    }

    if (err == ESP_OK) *object_count = count;
    free(summaries);
    object_store_internal_end_file_operation();
    return err;
''',
    'object profile summary scratch array',
)
if profile != profile_original:
    profile_path.write_text(profile, encoding='utf-8')
    print('Optimized object_store service stack usage.')
else:
    print('object_store service stack optimization already applied.')


control_path = Path('components/web_server/web_api_control.c')
control = control_path.read_text(encoding='utf-8')
control_original = control
control = replace_once(
    control,
    '#include <math.h>\n#include <stdio.h>\n#include <string.h>',
    '#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>',
    'web control stdlib include',
)
control = replace_once(
    control,
    '''static esp_err_t web_control_recording_start(const char *object_id)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_acquisition_is_running()) {
        esp_err_t err = seismic_acquisition_start();
        if (err != ESP_OK) {
            return err;
        }
    }

    return seismic_recorder_start(object_id);
}
''',
    '''static esp_err_t web_control_recording_start(const char *object_id)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = object_store_set_active(object_id);
    if (err != ESP_OK) {
        return err;
    }

    if (!seismic_acquisition_is_running()) {
        err = seismic_acquisition_start();
        if (err != ESP_OK) {
            return err;
        }
    }

    return seismic_recorder_start(object_id);
}
''',
    'recording active-object alignment',
)
control = replace_once(
    control,
    '''static esp_err_t web_api_control_handler(httpd_req_t *request)
{
    char body[WEB_CONTROL_MAX_BODY];
    esp_err_t err = web_control_read_body(request, body, sizeof(body));
    if (err != ESP_OK) return web_control_send_result(request, "INVALID", err, NULL);

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);
''',
    '''static esp_err_t web_api_control_handler(httpd_req_t *request)
{
    char *body = malloc(WEB_CONTROL_MAX_BODY);
    if (body == NULL) {
        return web_control_send_result(request, "INVALID", ESP_ERR_NO_MEM, NULL);
    }

    esp_err_t err = web_control_read_body(request, body, WEB_CONTROL_MAX_BODY);
    if (err != ESP_OK) {
        free(body);
        return web_control_send_result(request, "INVALID", err, NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) return web_control_send_result(request, "INVALID", ESP_ERR_INVALID_ARG, NULL);
''',
    'web control request scratch buffer',
)
if control != control_original:
    control_path.write_text(control, encoding='utf-8')
    print('Optimized web control task stack and recording object alignment.')
else:
    print('web control optimization already applied.')
