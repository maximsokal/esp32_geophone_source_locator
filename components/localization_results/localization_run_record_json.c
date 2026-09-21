#include "localization_run_record.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"


static const char *kind_name(
    localization_run_kind_t kind
)
{
    switch (kind) {
        case LOCALIZATION_RUN_KIND_JOINT_FUSION:
            return "joint_fusion";
        case LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD:
        default:
            return "individual_method";
    }
}


static bool kind_from_name(
    const char *name,
    localization_run_kind_t *kind
)
{
    if (name == NULL || kind == NULL) {
        return false;
    }
    if (strcmp(name, "individual_method") == 0) {
        *kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
        return true;
    }
    if (strcmp(name, "joint_fusion") == 0) {
        *kind = LOCALIZATION_RUN_KIND_JOINT_FUSION;
        return true;
    }
    return false;
}


static const char *dimensionality_name(
    localization_dimensionality_t dimensionality
)
{
    switch (dimensionality) {
        case LOCALIZATION_DIMENSIONALITY_PLANAR_2D:
            return "planar_2d";
        case LOCALIZATION_DIMENSIONALITY_CAPABLE_3D:
            return "capable_3d";
        case LOCALIZATION_DIMENSIONALITY_INSUFFICIENT:
        default:
            return "insufficient";
    }
}


static bool dimensionality_from_name(
    const char *name,
    localization_dimensionality_t *dimensionality
)
{
    if (name == NULL || dimensionality == NULL) {
        return false;
    }
    if (strcmp(name, "insufficient") == 0) {
        *dimensionality = LOCALIZATION_DIMENSIONALITY_INSUFFICIENT;
        return true;
    }
    if (strcmp(name, "planar_2d") == 0) {
        *dimensionality = LOCALIZATION_DIMENSIONALITY_PLANAR_2D;
        return true;
    }
    if (strcmp(name, "capable_3d") == 0) {
        *dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
        return true;
    }
    return false;
}


esp_err_t localization_run_record_to_json(
    const localization_run_record_t *record,
    char **json_out
)
{
    if (record == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!record->solved && record->rejection_reason[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        record->solved &&
        (
            !isfinite(record->source_position.x_m) ||
            !isfinite(record->source_position.y_m) ||
            !isfinite(record->source_position.z_m) ||
            !isfinite(record->rms_residual_us)
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ok =
        cJSON_AddNumberToObject(
            root, "schema_version", LOCALIZATION_RUN_SCHEMA_VERSION
        ) != NULL &&
        cJSON_AddStringToObject(root, "object_id", record->object_id) != NULL &&
        cJSON_AddStringToObject(root, "session_id", record->session_id) != NULL &&
        cJSON_AddNumberToObject(
            root, "computed_at_ms", (double)record->computed_at_ms
        ) != NULL &&
        cJSON_AddStringToObject(root, "kind", kind_name(record->kind)) != NULL &&
        cJSON_AddStringToObject(root, "method_name", record->method_name) != NULL &&
        cJSON_AddNumberToObject(
            root, "method_version", record->method_version
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "channel_mask_used", record->channel_mask_used
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "channel_mask_rejected", record->channel_mask_rejected
        ) != NULL &&
        cJSON_AddStringToObject(
            root, "dimensionality", dimensionality_name(record->dimensionality)
        ) != NULL &&
        cJSON_AddNumberToObject(
            root,
            "geometry_computed_at_ms",
            (double)record->geometry_computed_at_ms
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "propagation_speed_mps", record->propagation_speed_mps
        ) != NULL &&
        cJSON_AddBoolToObject(root, "solved", record->solved) != NULL;

    if (ok && record->solved) {
        cJSON *position = cJSON_AddObjectToObject(root, "source_position");
        ok = position != NULL &&
            cJSON_AddNumberToObject(position, "x_m", record->source_position.x_m) != NULL &&
            cJSON_AddNumberToObject(position, "y_m", record->source_position.y_m) != NULL &&
            cJSON_AddNumberToObject(position, "z_m", record->source_position.z_m) != NULL;

        ok = ok &&
            cJSON_AddNumberToObject(
                root, "emission_time_us", (double)record->emission_time_us
            ) != NULL &&
            cJSON_AddNumberToObject(
                root, "rms_residual_us", record->rms_residual_us
            ) != NULL &&
            cJSON_AddNumberToObject(root, "iterations", record->iterations) != NULL &&
            cJSON_AddBoolToObject(root, "converged", record->converged) != NULL;
    }

    if (ok && !record->solved) {
        ok = cJSON_AddStringToObject(
            root, "rejection_reason", record->rejection_reason
        ) != NULL;
    }

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *json_out = json;
    return ESP_OK;
}


static bool read_string(
    const cJSON *root,
    const char *name,
    char *out,
    size_t out_size
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (strlen(item->valuestring) + 1U > out_size) {
        return false;
    }
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1U);
    return true;
}


static bool read_number(
    const cJSON *root,
    const char *name,
    double *out
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}


static bool read_bool(
    const cJSON *root,
    const char *name,
    bool *out
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}


esp_err_t localization_run_record_from_json(
    const char *json,
    localization_run_record_t *record
)
{
    if (json == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(record, 0, sizeof(*record));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    double schema_version = 0.0;
    if (
        !read_number(root, "schema_version", &schema_version) ||
        schema_version < 0.0 ||
        floor(schema_version) != schema_version
    ) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((uint32_t)schema_version != LOCALIZATION_RUN_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    record->schema_version = (uint32_t)schema_version;

    char kind_buffer[24] = { 0 };
    char dimensionality_buffer[16] = { 0 };
    double computed_at_ms = 0.0;
    double method_version = 0.0;
    double channel_mask_used = 0.0;
    double channel_mask_rejected = 0.0;
    double geometry_computed_at_ms = 0.0;
    double propagation_speed_mps = 0.0;
    bool solved = false;

    bool ok =
        read_string(root, "object_id", record->object_id, sizeof(record->object_id)) &&
        read_string(root, "session_id", record->session_id, sizeof(record->session_id)) &&
        read_number(root, "computed_at_ms", &computed_at_ms) &&
        computed_at_ms >= 0.0 &&
        read_string(root, "kind", kind_buffer, sizeof(kind_buffer)) &&
        kind_from_name(kind_buffer, &record->kind) &&
        read_string(
            root, "method_name", record->method_name, sizeof(record->method_name)
        ) &&
        read_number(root, "method_version", &method_version) &&
        method_version >= 0.0 &&
        read_number(root, "channel_mask_used", &channel_mask_used) &&
        channel_mask_used >= 0.0 &&
        read_number(root, "channel_mask_rejected", &channel_mask_rejected) &&
        channel_mask_rejected >= 0.0 &&
        read_string(
            root,
            "dimensionality",
            dimensionality_buffer,
            sizeof(dimensionality_buffer)
        ) &&
        dimensionality_from_name(dimensionality_buffer, &record->dimensionality) &&
        read_number(root, "geometry_computed_at_ms", &geometry_computed_at_ms) &&
        geometry_computed_at_ms >= 0.0 &&
        read_number(root, "propagation_speed_mps", &propagation_speed_mps) &&
        read_bool(root, "solved", &solved);

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    record->computed_at_ms = (uint64_t)computed_at_ms;
    record->method_version = (uint32_t)method_version;
    record->channel_mask_used = (uint32_t)channel_mask_used;
    record->channel_mask_rejected = (uint32_t)channel_mask_rejected;
    record->geometry_computed_at_ms = (uint64_t)geometry_computed_at_ms;
    record->propagation_speed_mps = propagation_speed_mps;
    record->solved = solved;

    if (solved) {
        const cJSON *position =
            cJSON_GetObjectItemCaseSensitive(root, "source_position");
        double x_m = 0.0;
        double y_m = 0.0;
        double z_m = 0.0;
        double emission_time_us = 0.0;
        double rms_residual_us = 0.0;
        double iterations = 0.0;
        bool converged = false;

        ok = cJSON_IsObject(position) &&
            read_number(position, "x_m", &x_m) &&
            read_number(position, "y_m", &y_m) &&
            read_number(position, "z_m", &z_m) &&
            read_number(root, "emission_time_us", &emission_time_us) &&
            read_number(root, "rms_residual_us", &rms_residual_us) &&
            rms_residual_us >= 0.0 &&
            read_number(root, "iterations", &iterations) &&
            iterations >= 0.0 &&
            read_bool(root, "converged", &converged);

        if (!ok) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        record->source_position.x_m = x_m;
        record->source_position.y_m = y_m;
        record->source_position.z_m = z_m;
        record->emission_time_us = (int64_t)emission_time_us;
        record->rms_residual_us = rms_residual_us;
        record->iterations = (uint8_t)iterations;
        record->converged = converged;
    } else {
        ok = read_string(
            root,
            "rejection_reason",
            record->rejection_reason,
            sizeof(record->rejection_reason)
        ) && record->rejection_reason[0] != '\0';

        if (!ok) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
