#include "seismic_recorder_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage_manager.h"


static const char *TAG = "SEISMIC_RECORDER_JSON";

static seismic_recorder_recovery_request_callback_t s_recovery_request_callback = NULL;
static void *s_recovery_request_context = NULL;

static bool seismic_recorder_json_z_fallback_reason_known(const char *reason);

static bool seismic_recorder_json_cs_pair_state_known(const char *state)
{
    return state != NULL && (
        strcmp(state, "unknown") == 0 ||
        strcmp(state, "pending") == 0 ||
        strcmp(state, "valid") == 0 ||
        strcmp(state, "missing") == 0 ||
        strcmp(state, "retry_exhausted") == 0
    );
}

static bool seismic_recorder_json_cs_pair_failure_known(const char *failure)
{
    return failure != NULL && (
        strcmp(failure, "unknown") == 0 ||
        strcmp(failure, "none") == 0 ||
        strcmp(failure, "timeout") == 0 ||
        strcmp(failure, "transport") == 0 ||
        strcmp(failure, "outlier") == 0 ||
        strcmp(failure, "disconnected") == 0
    );
}


esp_err_t seismic_recorder_set_recovery_request_callback(
    seismic_recorder_recovery_request_callback_t callback,
    void *user_context
)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        s_recovery_request_callback != NULL &&
        s_recovery_request_callback != callback
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    s_recovery_request_callback = callback;
    s_recovery_request_context = user_context;
    return ESP_OK;
}


static esp_err_t seismic_recorder_json_metadata_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    esp_err_t err = seismic_recorder_fs_build_session_path(
        object_id,
        session_id,
        session_path,
        sizeof(session_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    const int written = snprintf(
        path,
        path_size,
        "%s/session.json",
        session_path
    );

    return
        written < 0 || written >= (int)path_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


static bool seismic_recorder_json_schema_version(
    const cJSON *root,
    uint32_t *version
)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(schema) || !isfinite(schema->valuedouble)) {
        return false;
    }
    if (
        schema->valuedouble != SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V1 &&
        schema->valuedouble != SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V2
    ) {
        return false;
    }
    if (version != NULL) {
        *version = (uint32_t)schema->valuedouble;
    }
    return true;
}


static esp_err_t seismic_recorder_json_read_root(
    const char *object_id,
    const char *session_id,
    cJSON **root_out,
    uint32_t *version_out
)
{
    if (object_id == NULL || session_id == NULL || root_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_out = NULL;

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = seismic_recorder_json_metadata_path(
        object_id, session_id, path, SEISMIC_RECORDER_PATH_SIZE
    );
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    char *buffer = malloc(SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    if (buffer == NULL) {
        free(path);
        return ESP_ERR_NO_MEM;
    }
    err = storage_manager_read_text(path, buffer, SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    free(path);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t version = 0U;
    if (!seismic_recorder_json_schema_version(root, &version)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *root_out = root;
    if (version_out != NULL) {
        *version_out = version;
    }
    return ESP_OK;
}

static bool seismic_recorder_json_samples_consistent(
    const char *object_id,
    const char *session_id,
    uint16_t format_version,
    uint32_t sample_rate_hz,
    uint32_t channel_count,
    uint64_t samples_written
)
{
    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    if (seismic_recorder_fs_build_session_path(
            object_id,
            session_id,
            session_path,
            sizeof(session_path)
        ) != ESP_OK) {
        return false;
    }

    char samples_path[SEISMIC_RECORDER_PATH_SIZE];
    const int written = snprintf(
        samples_path,
        sizeof(samples_path),
        "%s/samples.bin",
        session_path
    );
    if (written < 0 || written >= (int)sizeof(samples_path)) return false;

    FILE *file = fopen(samples_path, "rb");
    if (file == NULL) return false;

    struct stat info;
    seismic_recorder_binary_header_t header;
    const bool readable =
        stat(samples_path, &info) == 0 &&
        info.st_size >= 0 &&
        fread(&header, 1U, sizeof(header), file) == sizeof(header);
    const int close_result = fclose(file);
    if (!readable || close_result != 0) return false;

    const uint32_t expected_frame_size =
        format_version == SEISMIC_RECORDER_BINARY_VERSION_V1
            ? (uint32_t)sizeof(seismic_frame_t)
            : SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
    if (
        memcmp(header.magic, "GEOSEIS1", sizeof(header.magic)) != 0 ||
        header.version != format_version ||
        header.sample_rate_hz != sample_rate_hz ||
        header.channel_count != channel_count ||
        header.channel_count != SEISMIC_CHANNEL_COUNT ||
        header.frame_size_bytes != expected_frame_size ||
        header.reserved != 0U
    ) {
        return false;
    }

    const uint64_t record_size = header.frame_size_bytes;
    const uint64_t base_size = sizeof(header) +
        (format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
            ? sizeof(seismic_recorder_binary_v2_metadata_t)
            : 0U);
    const uint64_t block_count = format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
        ? samples_written / SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES +
            (samples_written % SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES != 0U ? 1U : 0U)
        : 0U;
    if (
        block_count >
            (UINT64_MAX - base_size) / sizeof(seismic_recorder_binary_v2_block_header_t)
    ) {
        return false;
    }
    const uint64_t minimum_size =
        base_size + block_count * sizeof(seismic_recorder_binary_v2_block_header_t);
    if (samples_written > (UINT64_MAX - minimum_size) / record_size) return false;

    const uint64_t required_size = minimum_size + samples_written * record_size;
    const uint64_t actual_size = (uint64_t)info.st_size;
    if (actual_size < required_size) return false;
    return format_version != SEISMIC_RECORDER_BINARY_VERSION_V1 ||
        (actual_size - base_size) % record_size == 0U;
}


static bool seismic_recorder_json_add_location(
    cJSON *root,
    const object_store_location_t *location
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "base_location");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", location->valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", location->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", location->longitude) != NULL &&
        cJSON_AddNumberToObject(object, "accuracy_m", location->accuracy_m) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", location->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", location->altitude_m) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_accuracy_m", location->altitude_accuracy_m) != NULL &&
        cJSON_AddNumberToObject(object, "captured_at_ms", (double)location->captured_at_ms) != NULL &&
        cJSON_AddStringToObject(object, "source", location->source) != NULL;
}


static bool seismic_recorder_json_add_cables(
    cJSON *root,
    const object_store_profile_t *profile
)
{
    cJSON *array = cJSON_AddArrayToObject(root, "cable_lengths_m");
    if (array == NULL) {
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *number = cJSON_CreateNumber(profile->cable_lengths_m[i]);
        if (number == NULL) {
            return false;
        }
        cJSON_AddItemToArray(array, number);
    }

    return true;
}


/*
 * MAX-166: freeze this recording's as-solved per-pod geometry into the
 * session's own metadata, separate from object.json. object_store_geometry_t
 * on the object can be recomputed/overwritten by a later recording of the
 * same object; this copy is what "this recording's sensor geometry" means
 * from now on, regardless of what the object's live geometry becomes later.
 */
static bool seismic_recorder_json_add_geometry(
    cJSON *root,
    const object_store_geometry_t *geometry,
    const seismic_recorder_session_participation_t *participation
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "geometry");
    if (object == NULL) {
        return false;
    }

    if (cJSON_AddBoolToObject(object, "anchored", geometry->anchored) == NULL) {
        return false;
    }

    if (participation->z_provenance.available) {
        const char *effective_policy =
            participation->z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_PLANAR
                ? "planar"
                : participation->z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
                    ? "barometric_relative"
                    : NULL;
        if (effective_policy == NULL ||
            strnlen(participation->z_provenance.fallback_reason,
                SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE) >= SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE ||
            !seismic_recorder_json_z_fallback_reason_known(
                participation->z_provenance.fallback_reason) ||
            (participation->z_provenance.effective_policy ==
                SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE &&
                participation->z_provenance.fallback_reason[0] != '\0') ||
            cJSON_AddStringToObject(object, "z_policy_effective", effective_policy) == NULL ||
            cJSON_AddStringToObject(object, "z_fallback_reason", participation->z_provenance.fallback_reason) == NULL) {
            return false;
        }
    }

    const seismic_recorder_geometry_provenance_t *solver =
        &participation->geometry_provenance;
    if (solver->available) {
        const char *ranging_source = solver->ranging_source[0] == '\0'
            ? "start_cycle"
            : solver->ranging_source;
        if (
            strnlen(solver->solver, sizeof(solver->solver)) >= sizeof(solver->solver) ||
            strnlen(solver->solver_version, sizeof(solver->solver_version)) >= sizeof(solver->solver_version) ||
            strnlen(solver->setup_id, sizeof(solver->setup_id)) >= sizeof(solver->setup_id) ||
            strnlen(ranging_source, SEISMIC_RECORDER_RANGING_SOURCE_SIZE) >=
                SEISMIC_RECORDER_RANGING_SOURCE_SIZE ||
            (strcmp(ranging_source, "workflow_ranging") != 0 &&
             strcmp(ranging_source, "start_cycle") != 0) ||
            strcmp(solver->solver, "nrf_geometry_solver") != 0 ||
            solver->solver_version[0] == '\0' ||
            solver->setup_id[0] == '\0'
        ) {
            return false;
        }
        cJSON *provenance = cJSON_AddObjectToObject(object, "provenance");
        if (
            provenance == NULL ||
            cJSON_AddStringToObject(provenance, "solver", solver->solver) == NULL ||
            cJSON_AddStringToObject(provenance, "solver_version", solver->solver_version) == NULL ||
            cJSON_AddBoolToObject(provenance, "refined", solver->refined) == NULL ||
            cJSON_AddNumberToObject(provenance, "refine_iterations", solver->refine_iterations) == NULL ||
            cJSON_AddNumberToObject(provenance, "topology_generation", solver->topology_generation) == NULL ||
            cJSON_AddNumberToObject(provenance, "cs_membership_generation", solver->cs_membership_generation) == NULL ||
            cJSON_AddNumberToObject(
                provenance,
                "main_height_reference_generation",
                solver->main_height_reference_generation
            ) == NULL ||
            cJSON_AddStringToObject(provenance, "setup_id", solver->setup_id) == NULL ||
            cJSON_AddNumberToObject(provenance, "start_uptime_ms", (double)solver->start_uptime_ms) == NULL ||
            cJSON_AddStringToObject(provenance, "ranging_source", ranging_source) == NULL
        ) {
            return false;
        }
    }

    cJSON *pods = cJSON_AddArrayToObject(object, "pods");
    if (pods == NULL) {
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        const object_store_pod_geometry_t *pod = &geometry->pods[i];

        cJSON *pod_object = cJSON_CreateObject();
        if (pod_object == NULL) {
            return false;
        }
        cJSON_AddItemToArray(pods, pod_object);

        if (
            cJSON_AddBoolToObject(pod_object, "computed", pod->computed) == NULL ||
            cJSON_AddBoolToObject(pod_object, "leveled", pod->leveled) == NULL ||
            cJSON_AddNumberToObject(pod_object, "x_mm", pod->x_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "y_mm", pod->y_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "z_mm", pod->z_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "residual_mm", pod->residual_mm) == NULL ||
            cJSON_AddNumberToObject(pod_object, "uncertainty_mm", pod->uncertainty_mm) == NULL ||
            cJSON_AddNumberToObject(
                pod_object,
                "computed_at_ms",
                (double)pod->computed_at_ms
            ) == NULL
        ) {
            return false;
        }

        const seismic_recorder_pod_participation_t *pod_participation =
            &participation->pods[i];
        if (
            cJSON_AddBoolToObject(
                pod_object,
                "assigned",
                pod_participation->assigned
            ) == NULL
        ) {
            return false;
        }
        if (pod_participation->node_id_known) {
            if (cJSON_AddNumberToObject(pod_object, "node_id", pod_participation->node_id) == NULL) {
                return false;
            }
        } else if (cJSON_AddNullToObject(pod_object, "node_id") == NULL) {
            return false;
        }

        if (pod_participation->hardware_id_length == 0U) {
            if (cJSON_AddNullToObject(pod_object, "hardware_id") == NULL) {
                return false;
            }
        } else {
            static const char hex[] = "0123456789abcdef";
            char hardware_id[SEISMIC_RECORDER_HARDWARE_ID_MAX * 2U + 1U];
            const size_t hardware_id_length =
                pod_participation->hardware_id_length > SEISMIC_RECORDER_HARDWARE_ID_MAX
                    ? SEISMIC_RECORDER_HARDWARE_ID_MAX
                    : pod_participation->hardware_id_length;
            for (size_t byte = 0U; byte < hardware_id_length; ++byte) {
                hardware_id[byte * 2U] = hex[pod_participation->hardware_id[byte] >> 4U];
                hardware_id[byte * 2U + 1U] = hex[pod_participation->hardware_id[byte] & 0x0FU];
            }
            hardware_id[hardware_id_length * 2U] = '\0';
            if (cJSON_AddStringToObject(pod_object, "hardware_id", hardware_id) == NULL) {
                return false;
            }
        }

        const char *position_source =
            pod_participation->position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING
                ? "channel_sounding"
                : pod_participation->position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO
                    ? "channel_sounding_baro"
                    : "none";
        if (
            pod_participation->position_source != SEISMIC_RECORDER_POSITION_SOURCE_NONE &&
            pod_participation->position_source != SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING &&
            pod_participation->position_source != SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO
        ) {
            return false;
        }
        if (cJSON_AddStringToObject(pod_object, "position_source", position_source) == NULL) {
            return false;
        }
        if (
            cJSON_AddNumberToObject(
                pod_object,
                "source_sample_uptime_ms",
                (double)pod_participation->source_sample_uptime_ms
            ) == NULL ||
            cJSON_AddNumberToObject(
                pod_object,
                "esp_receipt_uptime_ms",
                (double)pod_participation->esp_receipt_uptime_ms
            ) == NULL ||
            cJSON_AddNumberToObject(
                pod_object,
                "height_reference_generation",
                pod_participation->height_reference_generation
            ) == NULL
        ) {
            return false;
        }
        if (pod_participation->source_age_at_start_known) {
            if (cJSON_AddNumberToObject(
                pod_object,
                "source_age_ms_at_start",
                (double)pod_participation->source_age_ms_at_start
            ) == NULL) {
                return false;
            }
        } else if (cJSON_AddNullToObject(pod_object, "source_age_ms_at_start") == NULL) {
            return false;
        }
    }

    return true;
}


static const char *seismic_recorder_json_leveling_state_name(
    seismic_recorder_leveling_state_t state
)
{
    switch (state) {
        case SEISMIC_RECORDER_LEVELING_NO_REMOTES:
            return "no_remotes";
        case SEISMIC_RECORDER_LEVELING_INCOMPLETE:
            return "incomplete";
        case SEISMIC_RECORDER_LEVELING_ALL_ACCEPTED:
            return "all_accepted";
        case SEISMIC_RECORDER_LEVELING_UNKNOWN:
        default:
            return NULL;
    }
}


static bool seismic_recorder_json_add_recording_gate(
    cJSON *root,
    const seismic_recorder_gate_metadata_t *metadata
)
{
    if (!metadata->available) {
        return true;
    }
    const char *leveling_state =
        seismic_recorder_json_leveling_state_name(metadata->leveling_state);
    if (leveling_state == NULL) {
        return false;
    }

    cJSON *gate = cJSON_AddObjectToObject(root, "recording_gate");
    return gate != NULL &&
        cJSON_AddBoolToObject(gate, "gate_bypassed", metadata->gate_bypassed) != NULL &&
        cJSON_AddNumberToObject(
            gate, "assigned_remote_count", (double)metadata->assigned_remote_count) != NULL &&
        cJSON_AddNumberToObject(
            gate, "accepted_remote_count", (double)metadata->accepted_remote_count) != NULL &&
        cJSON_AddStringToObject(gate, "leveling_state", leveling_state) != NULL;
}


/*
 * MAX-148: freeze this recording's own CS pair evidence alongside its
 * geometry -- same rationale as seismic_recorder_json_add_geometry() above:
 * the object's live cs_pairs can be overwritten by a later recording, this
 * copy is what "this recording's placement confidence" means from now on.
 */
static bool seismic_recorder_json_add_cs_pairs(
    cJSON *root,
    const object_store_cs_pairs_t *cs_pairs
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "cs_pairs");
    if (object == NULL) {
        return false;
    }

    cJSON *pairs = cJSON_AddArrayToObject(object, "pairs");
    if (pairs == NULL) {
        return false;
    }

    for (size_t i = 0; i < cs_pairs->pair_count && i < OBJECT_STORE_MAX_CS_PAIRS; ++i) {
        const object_store_cs_pair_t *pair = &cs_pairs->pairs[i];
        if (
            strnlen(pair->state, sizeof(pair->state)) >= sizeof(pair->state) ||
            strnlen(pair->last_failure, sizeof(pair->last_failure)) >= sizeof(pair->last_failure)
        ) {
            return false;
        }
        const char *state = pair->state[0] == '\0' ? "unknown" : pair->state;
        const char *last_failure = pair->last_failure[0] == '\0' ? "unknown" : pair->last_failure;
        if (
            !seismic_recorder_json_cs_pair_state_known(state) ||
            !seismic_recorder_json_cs_pair_failure_known(last_failure)
        ) {
            return false;
        }

        cJSON *pair_object = cJSON_CreateObject();
        if (pair_object == NULL) {
            return false;
        }
        cJSON_AddItemToArray(pairs, pair_object);

        if (
            cJSON_AddNumberToObject(pair_object, "node_a", pair->node_a) == NULL ||
            cJSON_AddNumberToObject(pair_object, "node_b", pair->node_b) == NULL ||
            cJSON_AddBoolToObject(pair_object, "valid", pair->valid) == NULL ||
            cJSON_AddNumberToObject(pair_object, "distance_mm", (double)pair->distance_mm) == NULL ||
            cJSON_AddNumberToObject(pair_object, "confidence", pair->confidence) == NULL ||
            cJSON_AddNumberToObject(pair_object, "attempts", pair->attempts) == NULL ||
            cJSON_AddNumberToObject(pair_object, "valid_samples", pair->valid_samples) == NULL ||
            cJSON_AddNumberToObject(pair_object, "spread_mm", (double)pair->spread_mm) == NULL ||
            cJSON_AddNumberToObject(pair_object, "rejected_samples", pair->rejected_samples) == NULL ||
            cJSON_AddStringToObject(pair_object, "state", state) == NULL ||
            cJSON_AddStringToObject(pair_object, "last_failure", last_failure) == NULL ||
            cJSON_AddNumberToObject(pair_object, "provider_quality_flags", pair->provider_quality_flags) == NULL ||
            cJSON_AddNumberToObject(pair_object, "scheduler_quality_flags", pair->scheduler_quality_flags) == NULL
        ) {
            return false;
        }
    }

    return true;
}


static bool seismic_recorder_json_add_cadence(
    cJSON *root,
    const seismic_recorder_cadence_t *cadence
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "cadence");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "available", cadence->available) != NULL &&
        cJSON_AddNumberToObject(object, "timer_ticks", (double)cadence->timer_ticks) != NULL &&
        cJSON_AddNumberToObject(object, "conversion_attempts", (double)cadence->conversion_attempts) != NULL &&
        cJSON_AddNumberToObject(object, "collapsed_periods", (double)cadence->collapsed_periods) != NULL;
}


static bool seismic_recorder_json_status_is_known(
    const char *status
)
{
    return
        status != NULL &&
        (
            strcmp(status, "recording") == 0 ||
            strcmp(status, "complete") == 0 ||
            strcmp(status, "error") == 0 ||
            strcmp(status, "recovered_error") == 0
        );
}


esp_err_t seismic_recorder_json_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status,
    const seismic_recorder_session_participation_t *participation
)
{
    if (
        profile == NULL ||
        snapshot == NULL ||
        !seismic_recorder_json_status_is_known(status) ||
        snapshot->object_id[0] == '\0' ||
        snapshot->session_id[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t format_version = snapshot->format_version == 0U
        ? SEISMIC_RECORDER_BINARY_VERSION_V1
        : snapshot->format_version;
    if (
        format_version != SEISMIC_RECORDER_BINARY_VERSION_V1 &&
        format_version != SEISMIC_RECORDER_BINARY_VERSION_V2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_recorder_session_participation_t preserved_participation;
    if (participation == NULL) {
        const esp_err_t read_err = seismic_recorder_read_session_participation(
            snapshot->object_id,
            snapshot->session_id,
            &preserved_participation
        );
        if (read_err != ESP_OK) {
            return read_err;
        }
        participation = &preserved_participation;
    }

    const bool recovered = strcmp(status, "recovered_error") == 0;
    const bool recovered_metrics_available = !recovered;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ok =
        cJSON_AddNumberToObject(
            root,
            "schema_version",
            SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V2
        ) != NULL &&
        cJSON_AddStringToObject(root, "object_id", snapshot->object_id) != NULL &&
        cJSON_AddStringToObject(root, "object_name", profile->summary.name) != NULL &&
        cJSON_AddStringToObject(root, "session_id", snapshot->session_id) != NULL &&
        cJSON_AddStringToObject(root, "status", status) != NULL &&
        cJSON_AddStringToObject(
            root,
            "stop_stage",
            seismic_recorder_stop_stage_name(snapshot->stop_stage)
        ) != NULL &&
        cJSON_AddStringToObject(
            root,
            "stop_error_stage",
            seismic_recorder_stop_stage_name(snapshot->stop_error_stage)
        ) != NULL &&
        cJSON_AddNumberToObject(
            root,
            "stop_stage_error",
            snapshot->stop_stage_error
        ) != NULL &&
        cJSON_AddStringToObject(root, "source", snapshot->source_name) != NULL &&
        cJSON_AddStringToObject(
            root,
            "sample_format",
            format_version == SEISMIC_RECORDER_BINARY_VERSION_V2 ? "i16" : "f32"
        ) != NULL &&
        cJSON_AddNumberToObject(root, "format_version", format_version) != NULL &&
        cJSON_AddNumberToObject(root, "sample_rate_hz", snapshot->sample_rate_hz) != NULL &&
        cJSON_AddNumberToObject(root, "channel_count", snapshot->channel_count) != NULL &&
        cJSON_AddNumberToObject(root, "samples_written", (double)snapshot->samples_written) != NULL &&
        cJSON_AddBoolToObject(
            root,
            "dropped_frames_available",
            recovered_metrics_available
        ) != NULL &&
        cJSON_AddNumberToObject(root, "dropped_frames", (double)snapshot->dropped_frames) != NULL &&
        cJSON_AddBoolToObject(
            root,
            "recording_ms_available",
            recovered_metrics_available
        ) != NULL &&
        cJSON_AddNumberToObject(root, "recording_ms", (double)snapshot->recording_ms) != NULL &&
        cJSON_AddNumberToObject(root, "last_error", snapshot->last_error) != NULL;

    if (ok) {
        ok = seismic_recorder_json_add_cadence(root, &snapshot->cadence);
    }

    cJSON *format = NULL;
    if (ok) {
        format = cJSON_AddObjectToObject(root, "format");
        ok = format != NULL;
    }

    if (ok) {
        ok =
            cJSON_AddStringToObject(format, "container", "GEOSEIS1") != NULL &&
            cJSON_AddStringToObject(format, "file", "samples.bin") != NULL &&
            cJSON_AddStringToObject(
                format,
                "timestamp",
                format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
                    ? "block_first_timestamp_us_plus_sample_period"
                    : "int64_le_us"
            ) != NULL &&
            cJSON_AddStringToObject(
                format,
                "channels",
                format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
                    ? "int16_le_uv"
                    : "float32_le_uv"
            ) != NULL &&
            cJSON_AddNumberToObject(
                format,
                "frame_size_bytes",
                format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
                    ? SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
                    : sizeof(seismic_frame_t)
            ) != NULL;
    }

    if (ok) {
        ok = seismic_recorder_json_add_location(root, &profile->location);
    }
    if (ok) {
        ok = seismic_recorder_json_add_cables(root, profile);
    }
    if (ok) {
        ok = seismic_recorder_json_add_geometry(root, &profile->geometry, participation);
    }
    if (ok) {
        ok = seismic_recorder_json_add_recording_gate(
            root, &participation->recording_gate
        );
    }
    if (ok) {
        ok = seismic_recorder_json_add_cs_pairs(root, &profile->cs_pairs);
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

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        cJSON_free(json);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_json_metadata_path(
        snapshot->object_id,
        snapshot->session_id,
        path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    const int64_t write_started_us =
        esp_timer_get_time();
    if (err == ESP_OK) {
        err = storage_manager_atomic_write_text(path, json);
    }

    ESP_LOGI(
        TAG,
        "JSON_TIMING op=metadata-atomic-write result=%d elapsed_us=%lld",
        (int)err,
        (long long)(esp_timer_get_time() - write_started_us)
    );

    if (
        err != ESP_OK &&
        s_recovery_request_callback != NULL &&
        (
            strcmp(status, "complete") == 0 ||
            strcmp(status, "error") == 0
        )
    ) {
        s_recovery_request_callback(
            snapshot->object_id,
            snapshot->session_id,
            s_recovery_request_context
        );
    }

    free(path);
    cJSON_free(json);
    return err;
}


static bool seismic_recorder_json_number_uint64(const cJSON *item, uint64_t *value)
{
    if (
        value == NULL ||
        !cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble >= 18446744073709551616.0 ||
        floor(item->valuedouble) != item->valuedouble
    ) {
        return false;
    }

    *value = (uint64_t)item->valuedouble;
    return true;
}


static bool seismic_recorder_json_uint64(
    const cJSON *root,
    const char *name,
    uint64_t *value
)
{
    return value != NULL && seismic_recorder_json_number_uint64(
        cJSON_GetObjectItemCaseSensitive(root, name), value
    );
}


static bool seismic_recorder_json_optional_uint64(
    const cJSON *object,
    const char *name,
    uint64_t *value
)
{
    if (object == NULL || name == NULL || value == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        *value = 0U;
        return true;
    }
    return seismic_recorder_json_number_uint64(item, value);
}


static bool seismic_recorder_json_number_uint16(const cJSON *item, uint16_t *value)
{
    uint64_t parsed = 0U;
    if (
        value == NULL ||
        !seismic_recorder_json_number_uint64(item, &parsed) ||
        parsed > UINT16_MAX
    ) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}


static bool seismic_recorder_json_optional_uint16(
    const cJSON *object,
    const char *name,
    uint16_t *value
)
{
    if (object == NULL || name == NULL || value == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        *value = 0U;
        return true;
    }
    return seismic_recorder_json_number_uint16(item, value);
}


static bool seismic_recorder_json_number_uint32(const cJSON *item, uint32_t *value)
{
    uint64_t parsed = 0U;
    if (
        value == NULL ||
        !seismic_recorder_json_number_uint64(item, &parsed) ||
        parsed > UINT32_MAX
    ) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}


static bool seismic_recorder_json_uint32(
    const cJSON *root,
    const char *name,
    uint32_t *value
)
{
    return value != NULL && seismic_recorder_json_number_uint32(
        cJSON_GetObjectItemCaseSensitive(root, name), value
    );
}


static bool seismic_recorder_json_string_equals(
    const cJSON *root,
    const char *name,
    const char *expected
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return
        cJSON_IsString(item) &&
        item->valuestring != NULL &&
        strcmp(item->valuestring, expected) == 0;
}


static bool seismic_recorder_json_optional_bool(
    const cJSON *root,
    const char *name,
    bool default_value,
    bool *value
)
{
    if (root == NULL || name == NULL || value == NULL) {
        return false;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) {
        *value = default_value;
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *value = cJSON_IsTrue(item);
    return true;
}


static bool seismic_recorder_json_read_cadence(
    const cJSON *root,
    seismic_recorder_cadence_t *cadence
)
{
    if (root == NULL || cadence == NULL) {
        return false;
    }

    memset(cadence, 0, sizeof(*cadence));
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "cadence");
    if (object == NULL) {
        /* Schema v1 predates cadence evidence. Absence is unknown, not zero. */
        return true;
    }
    if (!cJSON_IsObject(object)) {
        return false;
    }

    const cJSON *available =
        cJSON_GetObjectItemCaseSensitive(object, "available");
    if (!cJSON_IsBool(available)) {
        return false;
    }
    if (!cJSON_IsTrue(available)) {
        return true;
    }

    uint64_t timer_ticks = 0U;
    uint64_t conversion_attempts = 0U;
    uint64_t collapsed_periods = 0U;
    if (
        !seismic_recorder_json_uint64(object, "timer_ticks", &timer_ticks) ||
        !seismic_recorder_json_uint64(object, "conversion_attempts", &conversion_attempts) ||
        !seismic_recorder_json_uint64(object, "collapsed_periods", &collapsed_periods) ||
        conversion_attempts > timer_ticks ||
        collapsed_periods != timer_ticks - conversion_attempts
    ) {
        return false;
    }

    cadence->available = true;
    cadence->timer_ticks = timer_ticks;
    cadence->conversion_attempts = conversion_attempts;
    cadence->collapsed_periods = collapsed_periods;
    return true;
}


static void seismic_recorder_json_parse_geometry(
    const cJSON *root,
    object_store_geometry_t *geometry
)
{
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    if (!cJSON_IsObject(node)) {
        return;
    }

    const cJSON *anchored = cJSON_GetObjectItemCaseSensitive(node, "anchored");
    geometry->anchored = cJSON_IsTrue(anchored);

    const cJSON *pods = cJSON_GetObjectItemCaseSensitive(node, "pods");
    if (!cJSON_IsArray(pods)) {
        return;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        const cJSON *pod = cJSON_GetArrayItem(pods, (int)i);
        if (!cJSON_IsObject(pod)) {
            continue;
        }
        const cJSON *computed = cJSON_GetObjectItemCaseSensitive(pod, "computed");
        if (!cJSON_IsTrue(computed)) {
            continue;
        }
        const cJSON *leveled = cJSON_GetObjectItemCaseSensitive(pod, "leveled");
        const cJSON *x_mm = cJSON_GetObjectItemCaseSensitive(pod, "x_mm");
        const cJSON *y_mm = cJSON_GetObjectItemCaseSensitive(pod, "y_mm");
        const cJSON *z_mm = cJSON_GetObjectItemCaseSensitive(pod, "z_mm");
        const cJSON *residual_mm = cJSON_GetObjectItemCaseSensitive(pod, "residual_mm");
        const cJSON *uncertainty_mm = cJSON_GetObjectItemCaseSensitive(pod, "uncertainty_mm");
        const cJSON *computed_at = cJSON_GetObjectItemCaseSensitive(pod, "computed_at_ms");
        if (!cJSON_IsNumber(x_mm) || !cJSON_IsNumber(y_mm) || !cJSON_IsNumber(z_mm)) {
            continue;
        }
        geometry->pods[i].computed = true;
        geometry->pods[i].leveled = cJSON_IsTrue(leveled);
        geometry->pods[i].x_mm = (int32_t)x_mm->valuedouble;
        geometry->pods[i].y_mm = (int32_t)y_mm->valuedouble;
        geometry->pods[i].z_mm = (int32_t)z_mm->valuedouble;
        geometry->pods[i].residual_mm =
            cJSON_IsNumber(residual_mm) && residual_mm->valuedouble >= 0.0 &&
            residual_mm->valuedouble <= (double)INT32_MAX
            ? (int32_t)residual_mm->valuedouble
            : 0;
        geometry->pods[i].uncertainty_mm =
            cJSON_IsNumber(uncertainty_mm) && uncertainty_mm->valuedouble >= 0.0 &&
            uncertainty_mm->valuedouble <= (double)INT32_MAX
            ? (int32_t)uncertainty_mm->valuedouble
            : 0;
        geometry->pods[i].computed_at_ms = cJSON_IsNumber(computed_at) && computed_at->valuedouble >= 0
            ? (uint64_t)computed_at->valuedouble
            : 0U;
    }
}


static int seismic_recorder_json_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}


static bool seismic_recorder_json_participation_pod_v2(
    const cJSON *pod_json,
    seismic_recorder_pod_participation_t *pod
)
{
    if (!cJSON_IsObject(pod_json) || pod == NULL) {
        return false;
    }

    const cJSON *assigned = cJSON_GetObjectItemCaseSensitive(pod_json, "assigned");
    const cJSON *node_id = cJSON_GetObjectItemCaseSensitive(pod_json, "node_id");
    const cJSON *hardware_id = cJSON_GetObjectItemCaseSensitive(pod_json, "hardware_id");
    const cJSON *position_source = cJSON_GetObjectItemCaseSensitive(pod_json, "position_source");
    if (
        !cJSON_IsBool(assigned) ||
        (!cJSON_IsNull(node_id) && !cJSON_IsNumber(node_id)) ||
        (!cJSON_IsNull(hardware_id) && !cJSON_IsString(hardware_id)) ||
        !cJSON_IsString(position_source) ||
        position_source->valuestring == NULL
    ) {
        return false;
    }

    seismic_recorder_pod_participation_t parsed = {0};
    parsed.assigned = cJSON_IsTrue(assigned);
    if (cJSON_IsNumber(node_id)) {
        if (
            !isfinite(node_id->valuedouble) ||
            node_id->valuedouble < 0.0 ||
            node_id->valuedouble > UINT8_MAX ||
            floor(node_id->valuedouble) != node_id->valuedouble
        ) {
            return false;
        }
        parsed.node_id_known = true;
        parsed.node_id = (uint8_t)node_id->valuedouble;
    }

    if (cJSON_IsString(hardware_id)) {
        const char *hex = hardware_id->valuestring;
        const size_t hex_length = strlen(hex);
        if (hex_length % 2U != 0U || hex_length / 2U > SEISMIC_RECORDER_HARDWARE_ID_MAX) {
            return false;
        }
        parsed.hardware_id_length = (uint8_t)(hex_length / 2U);
        for (size_t byte = 0U; byte < parsed.hardware_id_length; ++byte) {
            const int high = seismic_recorder_json_hex_nibble(hex[byte * 2U]);
            const int low = seismic_recorder_json_hex_nibble(hex[byte * 2U + 1U]);
            if (high < 0 || low < 0) {
                return false;
            }
            parsed.hardware_id[byte] = (uint8_t)((high << 4) | low);
        }
    }

    if (strcmp(position_source->valuestring, "none") == 0) {
        parsed.position_source = SEISMIC_RECORDER_POSITION_SOURCE_NONE;
    } else if (strcmp(position_source->valuestring, "channel_sounding") == 0) {
        parsed.position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
    } else if (strcmp(position_source->valuestring, "channel_sounding_baro") == 0) {
        parsed.position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO;
    } else {
        return false;
    }

    uint64_t source_sample_uptime_ms = 0U;
    uint64_t esp_receipt_uptime_ms = 0U;
    uint64_t source_age_ms_at_start = 0U;
    uint16_t height_reference_generation = 0U;
    const cJSON *source_age = cJSON_GetObjectItemCaseSensitive(
        pod_json, "source_age_ms_at_start"
    );
    if (
        !seismic_recorder_json_optional_uint64(
            pod_json, "source_sample_uptime_ms", &source_sample_uptime_ms
        ) ||
        !seismic_recorder_json_optional_uint64(
            pod_json, "esp_receipt_uptime_ms", &esp_receipt_uptime_ms
        ) ||
        !seismic_recorder_json_optional_uint16(
            pod_json, "height_reference_generation", &height_reference_generation
        )
    ) {
        return false;
    }
    if (source_age != NULL && !cJSON_IsNull(source_age)) {
        if (!seismic_recorder_json_number_uint64(source_age, &source_age_ms_at_start)) {
            return false;
        }
        parsed.source_age_at_start_known = true;
        parsed.source_age_ms_at_start = source_age_ms_at_start;
    }
    parsed.source_sample_uptime_ms = source_sample_uptime_ms;
    parsed.esp_receipt_uptime_ms = esp_receipt_uptime_ms;
    parsed.height_reference_generation = height_reference_generation;

    *pod = parsed;
    return true;
}


static bool seismic_recorder_json_parse_geometry_provenance(
    const cJSON *root,
    const object_store_geometry_t *geometry_data,
    seismic_recorder_geometry_provenance_t *provenance
)
{
    const cJSON *geometry = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    const cJSON *object = cJSON_IsObject(geometry)
        ? cJSON_GetObjectItemCaseSensitive(geometry, "provenance")
        : NULL;
    if (object == NULL) {
        return true;
    }
    if (!cJSON_IsObject(object) || provenance == NULL) {
        return false;
    }

    const cJSON *solver = cJSON_GetObjectItemCaseSensitive(object, "solver");
    const cJSON *solver_version = cJSON_GetObjectItemCaseSensitive(object, "solver_version");
    const cJSON *refined = cJSON_GetObjectItemCaseSensitive(object, "refined");
    const cJSON *setup_id = cJSON_GetObjectItemCaseSensitive(object, "setup_id");
    const cJSON *ranging_source = cJSON_GetObjectItemCaseSensitive(object, "ranging_source");
    uint16_t refine_iterations = 0U;
    uint32_t topology_generation = 0U;
    uint32_t cs_membership_generation = 0U;
    uint16_t main_height_reference_generation = 0U;
    uint64_t start_uptime_ms = 0U;
    if (
        !cJSON_IsString(solver) || solver->valuestring == NULL ||
        strnlen(solver->valuestring, SEISMIC_RECORDER_SOLVER_NAME_SIZE) >=
            SEISMIC_RECORDER_SOLVER_NAME_SIZE ||
        strcmp(solver->valuestring, "nrf_geometry_solver") != 0 ||
        !cJSON_IsString(solver_version) || solver_version->valuestring == NULL ||
        solver_version->valuestring[0] == '\0' ||
        strnlen(solver_version->valuestring, SEISMIC_RECORDER_SOLVER_VERSION_SIZE) >=
            SEISMIC_RECORDER_SOLVER_VERSION_SIZE ||
        !cJSON_IsBool(refined) ||
        !seismic_recorder_json_number_uint16(
            cJSON_GetObjectItemCaseSensitive(object, "refine_iterations"),
            &refine_iterations
        ) ||
        refine_iterations > UINT8_MAX ||
        !seismic_recorder_json_number_uint32(
            cJSON_GetObjectItemCaseSensitive(object, "topology_generation"),
            &topology_generation
        ) ||
        !seismic_recorder_json_number_uint32(
            cJSON_GetObjectItemCaseSensitive(object, "cs_membership_generation"),
            &cs_membership_generation
        ) ||
        !seismic_recorder_json_number_uint16(
            cJSON_GetObjectItemCaseSensitive(object, "main_height_reference_generation"),
            &main_height_reference_generation
        ) ||
        !cJSON_IsString(setup_id) || setup_id->valuestring == NULL ||
        setup_id->valuestring[0] == '\0' ||
        strnlen(setup_id->valuestring, SEISMIC_RECORDER_GEOMETRY_SETUP_ID_SIZE) >=
            SEISMIC_RECORDER_GEOMETRY_SETUP_ID_SIZE ||
        !seismic_recorder_json_uint64(object, "start_uptime_ms", &start_uptime_ms)
    ) {
        return false;
    }
    if (ranging_source != NULL &&
        (!cJSON_IsString(ranging_source) || ranging_source->valuestring == NULL ||
         strnlen(ranging_source->valuestring, SEISMIC_RECORDER_RANGING_SOURCE_SIZE) >=
             SEISMIC_RECORDER_RANGING_SOURCE_SIZE ||
         (strcmp(ranging_source->valuestring, "workflow_ranging") != 0 &&
          strcmp(ranging_source->valuestring, "start_cycle") != 0))) {
        return false;
    }

    uint64_t computed_at_ms = 0U;
    bool computed_at_found = false;
    if (geometry_data != NULL) {
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            if (geometry_data->pods[i].computed) {
                computed_at_ms = geometry_data->pods[i].computed_at_ms;
                computed_at_found = true;
                break;
            }
        }
    }
    char expected_setup_id[SEISMIC_RECORDER_GEOMETRY_SETUP_ID_SIZE];
    const int expected_setup_id_length = snprintf(
        expected_setup_id,
        sizeof(expected_setup_id),
        "%" PRIu64 "-%" PRIu32 "-%" PRIu32 "-%u",
        computed_at_ms,
        topology_generation,
        cs_membership_generation,
        (unsigned)main_height_reference_generation
    );
    if (
        !computed_at_found || expected_setup_id_length < 0 ||
        expected_setup_id_length >= (int)sizeof(expected_setup_id) ||
        strcmp(setup_id->valuestring, expected_setup_id) != 0
    ) {
        return false;
    }

    memset(provenance, 0, sizeof(*provenance));
    provenance->available = true;
    snprintf(provenance->solver, sizeof(provenance->solver), "%s", solver->valuestring);
    snprintf(
        provenance->solver_version,
        sizeof(provenance->solver_version),
        "%s",
        solver_version->valuestring
    );
    provenance->refined = cJSON_IsTrue(refined);
    provenance->refine_iterations = (uint8_t)refine_iterations;
    provenance->topology_generation = topology_generation;
    provenance->cs_membership_generation = cs_membership_generation;
    provenance->main_height_reference_generation = main_height_reference_generation;
    snprintf(provenance->setup_id, sizeof(provenance->setup_id), "%s", setup_id->valuestring);
    provenance->start_uptime_ms = start_uptime_ms;
    snprintf(
        provenance->ranging_source,
        sizeof(provenance->ranging_source),
        "%s",
        ranging_source == NULL ? "start_cycle" : ranging_source->valuestring
    );
    return true;
}


static bool seismic_recorder_json_z_fallback_reason_known(const char *reason)
{
    return reason != NULL && (
        reason[0] == '\0' ||
        strcmp(reason, "main_reference_invalid") == 0 ||
        strcmp(reason, "remote_reference_invalid") == 0 ||
        strcmp(reason, "reference_generation_mismatch") == 0
    );
}


static void seismic_recorder_json_parse_participation(
    const cJSON *root,
    uint32_t version,
    seismic_recorder_session_participation_t *participation
)
{
    const cJSON *recording_gate =
        cJSON_GetObjectItemCaseSensitive(root, "recording_gate");
    if (cJSON_IsObject(recording_gate)) {
        const cJSON *gate_bypassed =
            cJSON_GetObjectItemCaseSensitive(recording_gate, "gate_bypassed");
        const cJSON *assigned_remote_count =
            cJSON_GetObjectItemCaseSensitive(recording_gate, "assigned_remote_count");
        const cJSON *accepted_remote_count =
            cJSON_GetObjectItemCaseSensitive(recording_gate, "accepted_remote_count");
        const cJSON *leveling_state =
            cJSON_GetObjectItemCaseSensitive(recording_gate, "leveling_state");
        uint64_t assigned_count_value = 0U;
        uint64_t accepted_count_value = 0U;
        seismic_recorder_leveling_state_t parsed_state =
            SEISMIC_RECORDER_LEVELING_UNKNOWN;
        if (cJSON_IsBool(gate_bypassed) &&
            seismic_recorder_json_number_uint64(
                assigned_remote_count, &assigned_count_value) &&
            assigned_count_value <= UINT8_MAX &&
            seismic_recorder_json_number_uint64(
                accepted_remote_count, &accepted_count_value) &&
            accepted_count_value <= UINT8_MAX &&
            cJSON_IsString(leveling_state) && leveling_state->valuestring != NULL) {
            if (strcmp(leveling_state->valuestring, "no_remotes") == 0) {
                parsed_state = SEISMIC_RECORDER_LEVELING_NO_REMOTES;
            } else if (strcmp(leveling_state->valuestring, "incomplete") == 0) {
                parsed_state = SEISMIC_RECORDER_LEVELING_INCOMPLETE;
            } else if (strcmp(leveling_state->valuestring, "all_accepted") == 0) {
                parsed_state = SEISMIC_RECORDER_LEVELING_ALL_ACCEPTED;
            }
            if (parsed_state != SEISMIC_RECORDER_LEVELING_UNKNOWN) {
                participation->recording_gate.available = true;
                participation->recording_gate.gate_bypassed =
                    cJSON_IsTrue(gate_bypassed);
                participation->recording_gate.assigned_remote_count =
                    (uint8_t)assigned_count_value;
                participation->recording_gate.accepted_remote_count =
                    (uint8_t)accepted_count_value;
                participation->recording_gate.leveling_state = parsed_state;
            }
        }
    }

    object_store_geometry_t geometry = {0};
    seismic_recorder_json_parse_geometry(root, &geometry);
    const cJSON *geometry_node = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    const cJSON *pods = cJSON_IsObject(geometry_node)
        ? cJSON_GetObjectItemCaseSensitive(geometry_node, "pods")
        : NULL;

    const cJSON *effective_policy = cJSON_IsObject(geometry_node)
        ? cJSON_GetObjectItemCaseSensitive(geometry_node, "z_policy_effective")
        : NULL;
    const cJSON *fallback_reason = cJSON_IsObject(geometry_node)
        ? cJSON_GetObjectItemCaseSensitive(geometry_node, "z_fallback_reason")
        : NULL;
    const bool planar_policy = cJSON_IsString(effective_policy) && effective_policy->valuestring != NULL &&
        strcmp(effective_policy->valuestring, "planar") == 0;
    const bool barometric_policy = cJSON_IsString(effective_policy) && effective_policy->valuestring != NULL &&
        strcmp(effective_policy->valuestring, "barometric_relative") == 0;
    if (cJSON_IsString(effective_policy) && effective_policy->valuestring != NULL &&
        cJSON_IsString(fallback_reason) && fallback_reason->valuestring != NULL &&
        strnlen(fallback_reason->valuestring, SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE) <
            SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE &&
        seismic_recorder_json_z_fallback_reason_known(fallback_reason->valuestring) &&
        (planar_policy || (barometric_policy && fallback_reason->valuestring[0] == '\0'))) {
        participation->z_provenance.available = true;
        participation->z_provenance.effective_policy = barometric_policy
            ? SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
            : SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
        memcpy(participation->z_provenance.fallback_reason, fallback_reason->valuestring,
            strlen(fallback_reason->valuestring) + 1U);
    }

    if (version == SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V1) {
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            const bool computed = geometry.pods[i].computed;
            participation->pods[i].assigned = computed;
            participation->pods[i].position_source = computed
                ? SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING
                : SEISMIC_RECORDER_POSITION_SOURCE_NONE;
        }
    } else if (cJSON_IsArray(pods)) {
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            const cJSON *pod_json = cJSON_GetArrayItem(pods, (int)i);
            (void)seismic_recorder_json_participation_pod_v2(
                pod_json,
                &participation->pods[i]
            );
        }
    }

    if (version == SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V2) {
        seismic_recorder_geometry_provenance_t geometry_provenance = {0};
        if (seismic_recorder_json_parse_geometry_provenance(
            root, &geometry, &geometry_provenance
        )) {
            participation->geometry_provenance = geometry_provenance;
        }
    }
}


esp_err_t seismic_recorder_json_parse_participation_text(
    const char *json,
    seismic_recorder_session_participation_t *participation
)
{
    if (json == NULL || participation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(participation, 0, sizeof(*participation));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t version = 0U;
    if (!seismic_recorder_json_schema_version(root, &version)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    seismic_recorder_json_parse_participation(root, version, participation);
    cJSON_Delete(root);
    return ESP_OK;
}


static esp_err_t seismic_recorder_json_read_summary_impl(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary,
    bool primary_known_present
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        summary == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary, 0, sizeof(*summary));

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_json_metadata_path(
        object_id,
        session_id,
        path,
        SEISMIC_RECORDER_PATH_SIZE
    );
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    char *buffer = malloc(SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    if (buffer == NULL) {
        free(path);
        return ESP_ERR_NO_MEM;
    }

    err = primary_known_present
        ? storage_manager_read_text_known_present(
            path,
            buffer,
            SEISMIC_RECORDER_METADATA_BUFFER_SIZE
        )
        : storage_manager_read_text(
            path,
            buffer,
            SEISMIC_RECORDER_METADATA_BUFFER_SIZE
        );
    if (err != ESP_OK) {
        free(buffer);
        free(path);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        free(path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool valid =
        seismic_recorder_json_string_equals(
            root,
            "object_id",
            object_id
        ) &&
        seismic_recorder_json_string_equals(
            root,
            "session_id",
            session_id
        );

    valid = valid && seismic_recorder_json_schema_version(root, NULL);

    const cJSON *format_version_item =
        cJSON_GetObjectItemCaseSensitive(root, "format_version");
    uint16_t format_version = 0U;
    valid = valid && seismic_recorder_json_optional_uint16(
        root,
        "format_version",
        &format_version
    );
    const cJSON *sample_format =
        cJSON_GetObjectItemCaseSensitive(root, "sample_format");
    if (format_version_item == NULL) {
        format_version = SEISMIC_RECORDER_BINARY_VERSION_V1;
    }
    valid = valid && (
        format_version == SEISMIC_RECORDER_BINARY_VERSION_V1 ||
        format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
    );
    if (cJSON_IsString(sample_format) && sample_format->valuestring != NULL) {
        valid = valid && (
            (format_version == SEISMIC_RECORDER_BINARY_VERSION_V1 &&
             strcmp(sample_format->valuestring, "f32") == 0) ||
            (format_version == SEISMIC_RECORDER_BINARY_VERSION_V2 &&
             strcmp(sample_format->valuestring, "i16") == 0)
        );
    }
    else if (format_version_item != NULL) {
        valid = false;
    }
    else if (sample_format != NULL) {
        /* Existing v1 metadata used an object at this key. */
        valid = valid && format_version == SEISMIC_RECORDER_BINARY_VERSION_V1 &&
            cJSON_IsObject(sample_format);
    }

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const bool complete =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "complete") == 0;
    const bool errored =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "error") == 0;
    const bool recovered =
        cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "recovered_error") == 0;
    valid = valid && (complete || errored || recovered);

    const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
    valid = valid &&
        cJSON_IsString(source) &&
        source->valuestring != NULL &&
        source->valuestring[0] != '\0' &&
        strnlen(source->valuestring, SEISMIC_RECORDER_SOURCE_NAME_SIZE) <
            SEISMIC_RECORDER_SOURCE_NAME_SIZE;

    uint32_t sample_rate_hz = 0U;
    valid = valid &&
        seismic_recorder_json_uint32(
            root,
            "sample_rate_hz",
            &sample_rate_hz
        ) &&
        sample_rate_hz > 0U;

    uint32_t channel_count = 0U;
    valid = valid &&
        seismic_recorder_json_uint32(
            root,
            "channel_count",
            &channel_count
        ) &&
        channel_count == SEISMIC_CHANNEL_COUNT;

    uint64_t samples_written = 0U;
    uint64_t dropped_frames = 0U;
    uint64_t recording_ms = 0U;
    bool dropped_frames_available = true;
    bool recording_ms_available = true;
    seismic_recorder_cadence_t cadence = {0};
    valid = valid &&
        seismic_recorder_json_uint64(root, "samples_written", &samples_written) &&
        seismic_recorder_json_uint64(root, "dropped_frames", &dropped_frames) &&
        seismic_recorder_json_uint64(root, "recording_ms", &recording_ms) &&
        seismic_recorder_json_optional_bool(
            root,
            "dropped_frames_available",
            true,
            &dropped_frames_available
        ) &&
        seismic_recorder_json_optional_bool(
            root,
            "recording_ms_available",
            true,
            &recording_ms_available
        ) &&
        seismic_recorder_json_read_cadence(root, &cadence);

    if (
        recovered &&
        (dropped_frames_available || recording_ms_available || cadence.available)
    ) {
        valid = false;
    }

    if (!valid) {
        cJSON_Delete(root);
        free(path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const bool samples_consistent = seismic_recorder_json_samples_consistent(
        object_id,
        session_id,
        format_version,
        sample_rate_hz,
        channel_count,
        samples_written
    );

    snprintf(
        summary->session_id,
        sizeof(summary->session_id),
        "%s",
        session_id
    );
    snprintf(
        summary->source_name,
        sizeof(summary->source_name),
        "%s",
        source->valuestring
    );
    summary->status = !samples_consistent
        ? SEISMIC_RECORDER_SESSION_UNKNOWN
        : complete
            ? SEISMIC_RECORDER_SESSION_COMPLETE
            : recovered
                ? SEISMIC_RECORDER_SESSION_RECOVERED_ERROR
                : SEISMIC_RECORDER_SESSION_ERROR;
    summary->complete = complete && samples_consistent;
    summary->readable = samples_consistent;
    summary->recovered = recovered;
    summary->dropped_frames_available = dropped_frames_available;
    summary->recording_ms_available = recording_ms_available;
    summary->sample_rate_hz = sample_rate_hz;
    summary->channel_count = (uint8_t)channel_count;
    summary->samples_written = samples_written;
    summary->dropped_frames = dropped_frames;
    summary->recording_ms = recording_ms;
    summary->cadence = cadence;
    summary->format_version = format_version;
    summary->has_gaps = dropped_frames > 0U || !samples_consistent;
    summary->missing_samples = dropped_frames;

    cJSON_Delete(root);
    free(path);
    return ESP_OK;
}


esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary_impl(
        object_id,
        session_id,
        summary,
        false
    );
}


esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary_impl(
        object_id,
        session_id,
        summary,
        true
    );
}


/*
 * MAX-168: read back the exact per-session geometry snapshot MAX-166 froze
 * into session.json at recording start -- never the object's current live
 * geometry, which a later recording may have already overwritten. Mirrors
 * object_store_profile.c's object_store_read_geometry() parse logic against
 * this session's own JSON instead of object.json.
 *
 * A session.json that fails to open/parse is a real error (the session was
 * already confirmed to exist by the caller). A missing or malformed
 * "geometry" key inside an otherwise-valid session.json is not an error --
 * it just means every pod stays computed=false, matching a session recorded
 * before MAX-166 ever existed.
 */
esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    if (object_id == NULL || session_id == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(geometry, 0, sizeof(*geometry));
    cJSON *root = NULL;
    const esp_err_t err = seismic_recorder_json_read_root(
        object_id, session_id, &root, NULL
    );
    if (err != ESP_OK) {
        return err;
    }
    seismic_recorder_json_parse_geometry(root, geometry);
    cJSON_Delete(root);
    return ESP_OK;
}


esp_err_t seismic_recorder_read_session_participation(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_participation_t *participation
)
{
    if (object_id == NULL || session_id == NULL || participation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(participation, 0, sizeof(*participation));

    cJSON *root = NULL;
    uint32_t version = 0U;
    const esp_err_t err = seismic_recorder_json_read_root(
        object_id, session_id, &root, &version
    );
    if (err != ESP_OK) {
        return err;
    }
    seismic_recorder_json_parse_participation(root, version, participation);
    cJSON_Delete(root);
    return ESP_OK;
}


/*
 * MAX-148: read back the CS pair evidence frozen into session.json alongside
 * this session's geometry (see seismic_recorder_read_session_geometry()
 * above for the same "never the object's current live state" rationale).
 * Mirrors object_store_profile.c's object_store_read_cs_pairs() parse logic
 * against this session's own JSON instead of object.json. A missing or
 * malformed "cs_pairs" key is not an error -- it just means pair_count stays
 * 0, matching a session recorded before MAX-148 ever existed.
 */
esp_err_t seismic_recorder_read_session_cs_pairs(
    const char *object_id,
    const char *session_id,
    object_store_cs_pairs_t *cs_pairs
)
{
    if (object_id == NULL || session_id == NULL || cs_pairs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cs_pairs, 0, sizeof(*cs_pairs));

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_json_metadata_path(
        object_id,
        session_id,
        path,
        SEISMIC_RECORDER_PATH_SIZE
    );
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    char *buffer = malloc(SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    if (buffer == NULL) {
        free(path);
        return ESP_ERR_NO_MEM;
    }

    err = storage_manager_read_text(path, buffer, SEISMIC_RECORDER_METADATA_BUFFER_SIZE);
    free(path);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "cs_pairs");
    if (!cJSON_IsObject(node)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON *pairs = cJSON_GetObjectItemCaseSensitive(node, "pairs");
    if (!cJSON_IsArray(pairs)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    size_t count = 0U;
    const int array_size = cJSON_GetArraySize(pairs);
    for (int i = 0; i < array_size && count < OBJECT_STORE_MAX_CS_PAIRS; ++i) {
        cJSON *pair = cJSON_GetArrayItem(pairs, i);
        if (!cJSON_IsObject(pair)) {
            continue;
        }
        cJSON *node_a = cJSON_GetObjectItemCaseSensitive(pair, "node_a");
        cJSON *node_b = cJSON_GetObjectItemCaseSensitive(pair, "node_b");
        cJSON *valid = cJSON_GetObjectItemCaseSensitive(pair, "valid");
        cJSON *distance_mm = cJSON_GetObjectItemCaseSensitive(pair, "distance_mm");
        cJSON *confidence = cJSON_GetObjectItemCaseSensitive(pair, "confidence");
        cJSON *attempts = cJSON_GetObjectItemCaseSensitive(pair, "attempts");
        cJSON *valid_samples = cJSON_GetObjectItemCaseSensitive(pair, "valid_samples");
        if (
            !cJSON_IsNumber(node_a) || !cJSON_IsNumber(node_b) ||
            !cJSON_IsNumber(distance_mm) || !cJSON_IsNumber(confidence) ||
            !cJSON_IsNumber(attempts) || !cJSON_IsNumber(valid_samples)
        ) {
            continue;
        }
        object_store_cs_pair_t *out = &cs_pairs->pairs[count];
        out->node_a = (uint8_t)node_a->valuedouble;
        out->node_b = (uint8_t)node_b->valuedouble;
        out->valid = cJSON_IsTrue(valid);
        out->distance_mm = (int32_t)distance_mm->valuedouble;
        out->confidence = (uint16_t)confidence->valuedouble;
        out->attempts = (uint16_t)attempts->valuedouble;
        out->valid_samples = (uint8_t)valid_samples->valuedouble;
        /* Optional (MAX-206): absent in records written before the fields. */
        cJSON *spread_mm = cJSON_GetObjectItemCaseSensitive(pair, "spread_mm");
        cJSON *rejected_samples = cJSON_GetObjectItemCaseSensitive(pair, "rejected_samples");
        out->spread_mm =
            cJSON_IsNumber(spread_mm) && spread_mm->valuedouble >= 0.0 &&
            spread_mm->valuedouble <= (double)INT32_MAX
            ? (int32_t)spread_mm->valuedouble
            : 0;
        out->rejected_samples =
            cJSON_IsNumber(rejected_samples) && rejected_samples->valuedouble >= 0.0 &&
            rejected_samples->valuedouble <= 255.0
            ? (uint8_t)rejected_samples->valuedouble
            : 0U;

        const cJSON *state = cJSON_GetObjectItemCaseSensitive(pair, "state");
        const cJSON *last_failure = cJSON_GetObjectItemCaseSensitive(pair, "last_failure");
        const char *state_name = "unknown";
        const char *failure_name = "unknown";
        if (state != NULL) {
            if (
                !cJSON_IsString(state) || state->valuestring == NULL ||
                !seismic_recorder_json_cs_pair_state_known(state->valuestring)
            ) {
                continue;
            }
            state_name = state->valuestring;
        }
        if (last_failure != NULL) {
            if (
                !cJSON_IsString(last_failure) || last_failure->valuestring == NULL ||
                !seismic_recorder_json_cs_pair_failure_known(last_failure->valuestring)
            ) {
                continue;
            }
            failure_name = last_failure->valuestring;
        }
        if (
            strnlen(state_name, sizeof(out->state)) >= sizeof(out->state) ||
            strnlen(failure_name, sizeof(out->last_failure)) >= sizeof(out->last_failure) ||
            !seismic_recorder_json_optional_uint16(
                pair, "provider_quality_flags", &out->provider_quality_flags
            ) ||
            !seismic_recorder_json_optional_uint16(
                pair, "scheduler_quality_flags", &out->scheduler_quality_flags
            )
        ) {
            continue;
        }
        snprintf(out->state, sizeof(out->state), "%s", state_name);
        snprintf(out->last_failure, sizeof(out->last_failure), "%s", failure_name);
        ++count;
    }
    cs_pairs->pair_count = count;

    cJSON_Delete(root);
    return ESP_OK;
}
