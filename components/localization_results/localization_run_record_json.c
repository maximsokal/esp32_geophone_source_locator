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


static const char *failure_class_name(localization_failure_class_t failure_class)
{
    switch (failure_class) {
        case LOCALIZATION_FAILURE_CLASS_NONE: return "none";
        case LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN: return "record_again";
        case LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS: return "move_sensors";
        case LOCALIZATION_FAILURE_CLASS_INTERNAL: return "internal";
        default: return NULL;
    }
}


static bool failure_class_from_name(const char *name, localization_failure_class_t *failure_class)
{
    if (name == NULL || failure_class == NULL) {
        return false;
    }
    if (strcmp(name, "none") == 0) *failure_class = LOCALIZATION_FAILURE_CLASS_NONE;
    else if (strcmp(name, "record_again") == 0) *failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
    else if (strcmp(name, "move_sensors") == 0) *failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
    else if (strcmp(name, "internal") == 0) *failure_class = LOCALIZATION_FAILURE_CLASS_INTERNAL;
    else return false;
    return true;
}


/* MAX-301 (v5): names for the velocity range source and the method status. */
static const char *const s_velocity_source_names[] = {
    "unknown", "preset", "user", "scalar_derived", "measured_reserved",
};
static const char *const s_velocity_status_names[] = {
    "unknown", "not_applicable", "ok", "at_bound", "inconsistent",
};
#define VELOCITY_NAME_COUNT(names) (sizeof(names) / sizeof((names)[0]))


static bool velocity_name_index(
    const char *const *names, size_t count, const char *name, uint8_t *index_out
)
{
    if (name == NULL) return false;
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(names[i], name) == 0) {
            *index_out = (uint8_t)i;
            return true;
        }
    }
    return false;
}


/* No range (all zero, source UNKNOWN) is valid; a present range needs a
 * known source, finite 0 < min <= max, and a Vp/Vs pair set together with
 * 0 < min <= max. The calculate layer enforces the stricter policy limits. */
static bool velocity_range_valid(const localization_run_record_t *record)
{
    if (
        record->velocity_range_source >= VELOCITY_NAME_COUNT(s_velocity_source_names) ||
        record->velocity_status >= VELOCITY_NAME_COUNT(s_velocity_status_names)
    ) {
        return false;
    }
    const float min = record->velocity_range_min_mps;
    const float max = record->velocity_range_max_mps;
    const float r_min = record->velocity_range_vp_vs_min;
    const float r_max = record->velocity_range_vp_vs_max;
    if (!isfinite(min) || !isfinite(max) || !isfinite(r_min) || !isfinite(r_max)) return false;
    const bool has_range = min != 0.0f || max != 0.0f;
    if (!has_range) {
        return r_min == 0.0f && r_max == 0.0f &&
            record->velocity_range_source == LOCALIZATION_RUN_VELOCITY_SOURCE_UNKNOWN;
    }
    if (!(min > 0.0f && min <= max)) return false;
    if (r_min == 0.0f && r_max == 0.0f) return true;
    return r_min > 0.0f && r_min <= r_max;
}


static bool point_finite(const localization_point3d_t *p)
{
    return isfinite(p->x_m) && isfinite(p->y_m) && isfinite(p->z_m);
}


/* An absent uncertainty (credible_level 0) is valid; a present one must be
 * a level in (0, 1), finite non-negative sizes and an ordered box. */
static bool uncertainty_valid(const localization_run_uncertainty_t *u)
{
    if (u->credible_level == 0.0) {
        return true;
    }
    return
        isfinite(u->credible_level) && u->credible_level > 0.0 && u->credible_level < 1.0 &&
        point_finite(&u->std_m) && u->std_m.x_m >= 0.0 && u->std_m.y_m >= 0.0 && u->std_m.z_m >= 0.0 &&
        isfinite(u->credible_volume_m3) && u->credible_volume_m3 >= 0.0 &&
        isfinite(u->credible_area_m2) && u->credible_area_m2 >= 0.0 &&
        point_finite(&u->credible_min) && point_finite(&u->credible_max) &&
        isfinite(u->sigma_inflation) && (u->sigma_inflation == 0.0 || u->sigma_inflation >= 1.0) &&
        u->credible_min.x_m <= u->credible_max.x_m &&
        u->credible_min.y_m <= u->credible_max.y_m &&
        u->credible_min.z_m <= u->credible_max.z_m;
}


static bool add_point(cJSON *parent, const char *name, const localization_point3d_t *p)
{
    cJSON *object = cJSON_AddObjectToObject(parent, name);
    return object != NULL &&
        cJSON_AddNumberToObject(object, "x_m", p->x_m) != NULL &&
        cJSON_AddNumberToObject(object, "y_m", p->y_m) != NULL &&
        cJSON_AddNumberToObject(object, "z_m", p->z_m) != NULL;
}


esp_err_t localization_run_record_to_json(
    const localization_run_record_t *record,
    char **json_out
)
{
    if (record == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *failure_class = failure_class_name(record->failure_class);
    const bool legacy_unclassified_rejection = !record->solved &&
        record->failure_class == LOCALIZATION_FAILURE_CLASS_NONE &&
        record->schema_version == LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY;
    const uint32_t write_schema_version = legacy_unclassified_rejection
        ? LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY
        : LOCALIZATION_RUN_SCHEMA_VERSION;
    if (
        failure_class == NULL ||
        (record->uncertainty_too_high &&
         (!record->solved || record->failure_class != LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS)) ||
        (record->solved && !record->uncertainty_too_high &&
         record->failure_class != LOCALIZATION_FAILURE_CLASS_NONE) ||
        (!record->solved && record->failure_class == LOCALIZATION_FAILURE_CLASS_NONE &&
         record->schema_version != LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!record->solved && record->rejection_reason[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        !isfinite(record->propagation_speed_uncertainty_mps) ||
        record->propagation_speed_uncertainty_mps < 0.0 ||
        strnlen(record->material, sizeof(record->material)) >= sizeof(record->material) ||
        strnlen(record->analysis_window_reason, sizeof(record->analysis_window_reason)) >=
            sizeof(record->analysis_window_reason) ||
        (record->analysis_window_reason[0] != '\0' &&
         (!record->analysis_window_truncated ||
          strcmp(record->analysis_window_reason, "memory") != 0)) ||
        (record->analysis_window_truncated && record->analysis_window_frames == 0U)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!isfinite(record->confidence_score) || record->confidence_score < 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        !isfinite(record->estimated_p_velocity_mps) || record->estimated_p_velocity_mps < 0.0 ||
        !isfinite(record->estimated_s_velocity_mps) || record->estimated_s_velocity_mps < 0.0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (record->solved && !uncertainty_valid(&record->uncertainty)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!velocity_range_valid(record)) {
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
            root, "schema_version", write_schema_version
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
        cJSON_AddStringToObject(
            root, "propagation_model_name", record->propagation_model_name
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "propagation_speed_mps", record->propagation_speed_mps
        ) != NULL &&
        cJSON_AddNumberToObject(
            root,
            "propagation_speed_uncertainty_mps",
            record->propagation_speed_uncertainty_mps
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "confidence_score", record->confidence_score
        ) != NULL &&
        cJSON_AddNumberToObject(
            root, "degrees_of_freedom", (double)record->degrees_of_freedom
        ) != NULL &&
        cJSON_AddBoolToObject(root, "solved", record->solved) != NULL &&
        cJSON_AddStringToObject(root, "failure_class", failure_class) != NULL &&
        cJSON_AddBoolToObject(root, "uncertainty_too_high", record->uncertainty_too_high) != NULL;

    if (ok && record->material[0] != '\0') {
        ok = cJSON_AddStringToObject(root, "material", record->material) != NULL;
    }

    /* MAX-301 (v5): written only when present, so a record without a range
     * serializes exactly as before apart from its schema number. */
    if (ok && (record->velocity_range_min_mps != 0.0f || record->velocity_range_max_mps != 0.0f)) {
        cJSON *range = cJSON_AddObjectToObject(root, "velocity_range");
        ok = range != NULL &&
            cJSON_AddNumberToObject(range, "v_min_mps", record->velocity_range_min_mps) != NULL &&
            cJSON_AddNumberToObject(range, "v_max_mps", record->velocity_range_max_mps) != NULL &&
            cJSON_AddNumberToObject(range, "vp_vs_min", record->velocity_range_vp_vs_min) != NULL &&
            cJSON_AddNumberToObject(range, "vp_vs_max", record->velocity_range_vp_vs_max) != NULL &&
            cJSON_AddStringToObject(
                range, "source", s_velocity_source_names[record->velocity_range_source]
            ) != NULL;
    }
    if (ok && record->velocity_status != LOCALIZATION_RUN_VELOCITY_STATUS_UNKNOWN) {
        ok = cJSON_AddStringToObject(
            root, "velocity_status", s_velocity_status_names[record->velocity_status]
        ) != NULL;
    }

    if (ok && record->analysis_window_truncated) {
        cJSON *window = cJSON_AddObjectToObject(root, "analysis_window");
        ok = window != NULL &&
            cJSON_AddNumberToObject(
                window, "start_index", (double)record->analysis_window_start_index
            ) != NULL &&
            cJSON_AddNumberToObject(
                window, "frames", (double)record->analysis_window_frames
            ) != NULL &&
            cJSON_AddBoolToObject(window, "truncated", true) != NULL;
        if (ok && record->analysis_window_reason[0] != '\0') {
            ok = cJSON_AddStringToObject(
                window, "reason", record->analysis_window_reason
            ) != NULL;
        }
    }

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

        /* Optional: only velocity-free methods estimate velocities. */
        if (ok && record->estimated_p_velocity_mps > 0.0) {
            ok = cJSON_AddNumberToObject(
                root, "estimated_p_velocity_mps", record->estimated_p_velocity_mps
            ) != NULL;
        }
        if (ok && record->estimated_s_velocity_mps > 0.0) {
            ok = cJSON_AddNumberToObject(
                root, "estimated_s_velocity_mps", record->estimated_s_velocity_mps
            ) != NULL;
        }
        /* Optional: only methods with a posterior report an uncertainty. */
        if (ok && record->uncertainty.credible_level > 0.0) {
            const localization_run_uncertainty_t *u = &record->uncertainty;
            cJSON *uncertainty = cJSON_AddObjectToObject(root, "uncertainty");
            ok = uncertainty != NULL &&
                add_point(uncertainty, "std_m", &u->std_m) &&
                cJSON_AddNumberToObject(uncertainty, "credible_level", u->credible_level) != NULL &&
                cJSON_AddNumberToObject(uncertainty, "credible_volume_m3", u->credible_volume_m3) != NULL &&
                cJSON_AddNumberToObject(uncertainty, "credible_area_m2", u->credible_area_m2) != NULL &&
                add_point(uncertainty, "credible_min", &u->credible_min) &&
                add_point(uncertainty, "credible_max", &u->credible_max) &&
                cJSON_AddNumberToObject(
                    uncertainty, "sigma_inflation", u->sigma_inflation >= 1.0 ? u->sigma_inflation : 1.0
                ) != NULL;
        }
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


static bool read_point(const cJSON *parent, const char *name, localization_point3d_t *p)
{
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsObject(object) &&
        read_number(object, "x_m", &p->x_m) &&
        read_number(object, "y_m", &p->y_m) &&
        read_number(object, "z_m", &p->z_m);
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


/* MAX-301 (v5): optional "velocity_range" object and "velocity_status"
 * name. Absent -> the zeroed record stays UNKNOWN / no range. Present but
 * malformed -> false (the whole record is rejected). */
static bool read_velocity_range(const cJSON *root, localization_run_record_t *record)
{
    const cJSON *range = cJSON_GetObjectItemCaseSensitive(root, "velocity_range");
    if (range != NULL) {
        double min = 0.0;
        double max = 0.0;
        double r_min = 0.0;
        double r_max = 0.0;
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(range, "source");
        if (
            !cJSON_IsObject(range) ||
            !read_number(range, "v_min_mps", &min) ||
            !read_number(range, "v_max_mps", &max) ||
            !read_number(range, "vp_vs_min", &r_min) ||
            !read_number(range, "vp_vs_max", &r_max) ||
            !cJSON_IsString(source) ||
            !velocity_name_index(
                s_velocity_source_names, VELOCITY_NAME_COUNT(s_velocity_source_names),
                source->valuestring, &record->velocity_range_source)
        ) {
            return false;
        }
        record->velocity_range_min_mps = (float)min;
        record->velocity_range_max_mps = (float)max;
        record->velocity_range_vp_vs_min = (float)r_min;
        record->velocity_range_vp_vs_max = (float)r_max;
    }
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "velocity_status");
    if (status != NULL) {
        if (
            !cJSON_IsString(status) ||
            !velocity_name_index(
                s_velocity_status_names, VELOCITY_NAME_COUNT(s_velocity_status_names),
                status->valuestring, &record->velocity_status)
        ) {
            return false;
        }
    }
    return velocity_range_valid(record);
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
    /* Readable: v5 (current, MAX-301 velocity range), v4 and legacy v3. */
    if (
        (uint32_t)schema_version != LOCALIZATION_RUN_SCHEMA_VERSION &&
        (uint32_t)schema_version != LOCALIZATION_RUN_SCHEMA_VERSION_V4 &&
        (uint32_t)schema_version != LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY
    ) {
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
    double propagation_speed_uncertainty_mps = 0.0;
    double confidence_score = 0.0;
    bool solved = false;
    bool uncertainty_too_high = false;

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
        read_string(
            root,
            "propagation_model_name",
            record->propagation_model_name,
            sizeof(record->propagation_model_name)
        ) &&
        read_number(root, "propagation_speed_mps", &propagation_speed_mps) &&
        read_number(
            root,
            "propagation_speed_uncertainty_mps",
            &propagation_speed_uncertainty_mps
        ) &&
        propagation_speed_uncertainty_mps >= 0.0 &&
        read_number(root, "confidence_score", &confidence_score) &&
        confidence_score >= 0.0 &&
        read_bool(root, "solved", &solved);

    const cJSON *material_item = cJSON_GetObjectItemCaseSensitive(root, "material");
    if (ok && material_item != NULL && !cJSON_IsNull(material_item)) {
        ok = read_string(root, "material", record->material, sizeof(record->material));
    }

    const cJSON *analysis_window =
        cJSON_GetObjectItemCaseSensitive(root, "analysis_window");
    if (ok && analysis_window != NULL) {
        double start_index = 0.0;
        double frames = 0.0;
        bool truncated = false;
        ok = cJSON_IsObject(analysis_window) &&
            read_number(analysis_window, "start_index", &start_index) &&
            start_index >= 0.0 && floor(start_index) == start_index &&
            start_index <= 9007199254740991.0 &&
            read_number(analysis_window, "frames", &frames) &&
            frames > 0.0 && floor(frames) == frames &&
            frames <= 9007199254740991.0 &&
            read_bool(analysis_window, "truncated", &truncated) && truncated;
        if (ok) {
            record->analysis_window_truncated = true;
            record->analysis_window_start_index = (uint64_t)start_index;
            record->analysis_window_frames = (uint64_t)frames;
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(
                analysis_window, "reason"
            );
            if (reason != NULL) {
                ok = read_string(
                    analysis_window, "reason", record->analysis_window_reason,
                    sizeof(record->analysis_window_reason)
                );
            }
        }
    }

    const cJSON *failure_class_item = cJSON_GetObjectItemCaseSensitive(root, "failure_class");
    if (failure_class_item != NULL) {
        ok = ok && cJSON_IsString(failure_class_item) &&
            failure_class_from_name(failure_class_item->valuestring, &record->failure_class);
    } else if ((uint32_t)schema_version >= LOCALIZATION_RUN_SCHEMA_VERSION_V4) {
        ok = false;
    }
    const cJSON *uncertainty_too_high_item = cJSON_GetObjectItemCaseSensitive(root, "uncertainty_too_high");
    if (uncertainty_too_high_item != NULL) {
        ok = ok && read_bool(root, "uncertainty_too_high", &uncertainty_too_high);
    }

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
    record->propagation_speed_uncertainty_mps = propagation_speed_uncertainty_mps;
    record->confidence_score = confidence_score;
    record->solved = solved;
    record->uncertainty_too_high = uncertainty_too_high;

    /* Optional (MAX-212): absent in records written before the field. */
    double degrees_of_freedom = -1.0;
    if (
        !read_number(root, "degrees_of_freedom", &degrees_of_freedom) ||
        degrees_of_freedom < -1.0 ||
        degrees_of_freedom > 64.0
    ) {
        degrees_of_freedom = -1.0;
    }
    record->degrees_of_freedom = (int32_t)degrees_of_freedom;

    /* MAX-301 (v5): optional; absent in v3/v4 records, which read as UNKNOWN. */
    if (!read_velocity_range(root, record)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

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

        /* Optional: absent (0) for methods that use a given velocity and in
         * records written before the fields existed. */
        double estimated = 0.0;
        if (read_number(root, "estimated_p_velocity_mps", &estimated) && estimated > 0.0) {
            record->estimated_p_velocity_mps = estimated;
        }
        estimated = 0.0;
        if (read_number(root, "estimated_s_velocity_mps", &estimated) && estimated > 0.0) {
            record->estimated_s_velocity_mps = estimated;
        }

        /* Optional; when present it must be complete and consistent. */
        const cJSON *uncertainty = cJSON_GetObjectItemCaseSensitive(root, "uncertainty");
        if (uncertainty != NULL) {
            localization_run_uncertainty_t *u = &record->uncertainty;
            ok = cJSON_IsObject(uncertainty) &&
                read_point(uncertainty, "std_m", &u->std_m) &&
                read_number(uncertainty, "credible_level", &u->credible_level) &&
                read_number(uncertainty, "credible_volume_m3", &u->credible_volume_m3) &&
                read_number(uncertainty, "credible_area_m2", &u->credible_area_m2) &&
                read_point(uncertainty, "credible_min", &u->credible_min) &&
                read_point(uncertainty, "credible_max", &u->credible_max) &&
                u->credible_level > 0.0;
            /* Optional inside the object (records before it existed). */
            if (ok && !read_number(uncertainty, "sigma_inflation", &u->sigma_inflation)) {
                u->sigma_inflation = 1.0;
            }
            ok = ok && uncertainty_valid(u);
            if (!ok) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
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
        if (
            (uint32_t)schema_version >= LOCALIZATION_RUN_SCHEMA_VERSION_V4 &&
            record->failure_class == LOCALIZATION_FAILURE_CLASS_NONE
        ) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (
        (record->uncertainty_too_high &&
         (!record->solved || record->failure_class != LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS ||
          record->uncertainty.credible_level <= 0.0)) ||
        (record->solved && !record->uncertainty_too_high &&
         record->failure_class != LOCALIZATION_FAILURE_CLASS_NONE)
    ) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON_Delete(root);
    return ESP_OK;
}
