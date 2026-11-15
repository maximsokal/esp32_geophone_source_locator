#include "localization_run_store.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MAX132_CONFIG_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MAX132_CONFIG_MKDIR(path) mkdir(path, 0775)
#endif

#include "storage_manager.h"

static void ensure_parent(const char *path)
{
    char parent[LOCALIZATION_RUN_STORE_PATH_MAX];
    const size_t length = strlen(path);
    assert(length > 0U && length < sizeof(parent));
    memcpy(parent, path, length + 1U);
    char *separator = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    assert(separator != NULL);
    *separator = '\0';
    for (char *p = parent + 1; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            const char saved = *p;
            *p = '\0';
            if (MAX132_CONFIG_MKDIR(parent) != 0) assert(errno == EEXIST);
            *p = saved;
        }
    }
    if (MAX132_CONFIG_MKDIR(parent) != 0) assert(errno == EEXIST);
}

/* The run store lists a session under the storage operation guard; the host has no card to guard. */
esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return ESP_ERR_NOT_FOUND;
    const size_t count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool truncated = fgetc(file) != EOF;
    (void)fclose(file);
    if (truncated) return ESP_ERR_INVALID_SIZE;
    buffer[count] = '\0';
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) return ESP_ERR_INVALID_ARG;
    ensure_parent(path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return ESP_FAIL;
    const bool wrote = fputs(text, file) >= 0;
    return fclose(file) == 0 && wrote ? ESP_OK : ESP_FAIL;
}

void max132_config_store_seed_saved_result(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    (void)snprintf(record.object_id, sizeof(record.object_id), "%s", "OBJ_0001");
    (void)snprintf(record.session_id, sizeof(record.session_id), "%s", "S600001");
    record.computed_at_ms = 6000001U;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    (void)snprintf(record.method_name, sizeof(record.method_name), "%s", "tdoa_gauss_newton");
    record.method_version = 1U;
    record.channel_mask_used = 0x0FU;
    record.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    (void)snprintf(record.material, sizeof(record.material), "%s", "custom");
    record.propagation_speed_mps = 300.0;
    record.propagation_speed_uncertainty_mps = 10.0;
    record.solved = true;
    record.confidence_score = 0.5;
    record.degrees_of_freedom = 1;
    record.source_position = (localization_point3d_t){ 1.0, 2.0, 3.0 };
    record.rms_residual_us = 5.0;
    record.converged = true;
    assert(localization_run_store_write("OBJ_0001", "S600001", &record) == ESP_OK);
}

void max132_config_store_assert_prior_result(void)
{
    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(localization_run_store_list("OBJ_0001", "S600001", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count) == ESP_OK);
    assert(count == 1U);
    assert(records[0].solved);
    assert(records[0].computed_at_ms == 6000001U);
    assert(strcmp(records[0].method_name, "tdoa_gauss_newton") == 0);
}
