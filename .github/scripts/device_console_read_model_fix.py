from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f'Expected fragment not found for {label}:\n{old}')
    return text.replace(old, new, 1)


header_path = Path('components/object_store/include/object_store.h')
header = header_path.read_text(encoding='utf-8')
header_original = header
header = replace_once(
    header,
    '''esp_err_t object_store_list_profiles(object_store_profile_t *objects, size_t max_objects, size_t *object_count);\n''',
    '''esp_err_t object_store_list_profiles(object_store_profile_t *objects, size_t max_objects, size_t *object_count);\nesp_err_t object_store_list_profiles_with_defaults(\n    object_store_profile_t *objects,\n    size_t max_objects,\n    size_t *object_count,\n    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]\n);\n''',
    'object read-model API',
)
if header != header_original:
    header_path.write_text(header, encoding='utf-8')
    print('Added combined object profiles/defaults read-model API.')


profile_path = Path('components/object_store/object_store_profile.c')
profile = profile_path.read_text(encoding='utf-8')
profile_original = profile
profile = replace_once(
    profile,
    '''esp_err_t object_store_list_profiles(\n    object_store_profile_t *objects,\n    size_t max_objects,\n    size_t *object_count\n)\n{\n    if (objects == NULL || object_count == NULL || max_objects == 0) {\n        return ESP_ERR_INVALID_ARG;\n    }\n''',
    '''esp_err_t object_store_list_profiles_with_defaults(\n    object_store_profile_t *objects,\n    size_t max_objects,\n    size_t *object_count,\n    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]\n)\n{\n    if (\n        objects == NULL ||\n        object_count == NULL ||\n        default_cable_lengths_m == NULL ||\n        max_objects == 0\n    ) {\n        return ESP_ERR_INVALID_ARG;\n    }\n''',
    'combined object list implementation signature',
)
profile = replace_once(
    profile,
    '''    float defaults[OBJECT_STORE_POD_COUNT];\n    if (object_store_settings_load_locked(defaults) != ESP_OK) {\n        object_store_default_cables(defaults);\n    }\n\n    if (err == ESP_OK) {\n''',
    '''    if (object_store_settings_load_locked(default_cable_lengths_m) != ESP_OK) {\n        object_store_default_cables(default_cable_lengths_m);\n    }\n\n    if (err == ESP_OK) {\n''',
    'single settings read into output defaults',
)
profile = replace_once(
    profile,
    '''                defaults,\n                &objects[i]\n''',
    '''                default_cable_lengths_m,\n                &objects[i]\n''',
    'reuse output defaults for profiles',
)
profile = replace_once(
    profile,
    '''    free(summaries);\n    object_store_internal_end_file_operation();\n    return err;\n}\n\nstatic esp_err_t object_store_mutation_begin''',
    '''    free(summaries);\n    object_store_internal_end_file_operation();\n    return err;\n}\n\nesp_err_t object_store_list_profiles(\n    object_store_profile_t *objects,\n    size_t max_objects,\n    size_t *object_count\n)\n{\n    float defaults[OBJECT_STORE_POD_COUNT];\n    return object_store_list_profiles_with_defaults(\n        objects,\n        max_objects,\n        object_count,\n        defaults\n    );\n}\n\nstatic esp_err_t object_store_mutation_begin''',
    'legacy list wrapper',
)
if profile != profile_original:
    profile_path.write_text(profile, encoding='utf-8')
    print('Made object profiles/defaults share one settings read and storage operation.')


json_path = Path('components/web_server/web_status_json_objects.c')
json_text = json_path.read_text(encoding='utf-8')
json_original = json_text
json_text = replace_once(
    json_text,
    '''    size_t count = 0;\n    esp_err_t err = object_store_list_profiles(\n        profiles,\n        OBJECT_STORE_MAX_OBJECTS,\n        &count\n    );\n''',
    '''    size_t count = 0;\n    float defaults[OBJECT_STORE_POD_COUNT];\n    esp_err_t err = object_store_list_profiles_with_defaults(\n        profiles,\n        OBJECT_STORE_MAX_OBJECTS,\n        &count,\n        defaults\n    );\n''',
    'web object combined read model',
)
json_text = replace_once(
    json_text,
    '''    float defaults[OBJECT_STORE_POD_COUNT];\n    if (object_store_get_cable_defaults(defaults) != ESP_OK) {\n        for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {\n            defaults[i] = OBJECT_STORE_DEFAULT_CABLE_LENGTH_M;\n        }\n    }\n\n    cJSON *default_array = cJSON_AddArrayToObject(store, "default_cable_lengths_m");\n''',
    '''    cJSON *default_array = cJSON_AddArrayToObject(store, "default_cable_lengths_m");\n''',
    'remove second settings read from status JSON',
)
if json_text != json_original:
    json_path.write_text(json_text, encoding='utf-8')
    print('Removed second settings.json read from ?objects=1 status response.')

print('Combined object read-model fix complete.')
