#include "localization_pick_record.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"


static const char *pick_status_name(
    geophone_pick_status_t status
)
{
    switch (status) {
        case GEOPHONE_PICK_ACCEPTED:
            return "accepted";
        case GEOPHONE_PICK_REJECTED_NO_TRIGGER:
            return "rejected_no_trigger";
        case GEOPHONE_PICK_REJECTED_NO_REFINEMENT:
            return "rejected_no_refinement";
        case GEOPHONE_PICK_REJECTED_LOW_SNR:
            return "rejected_low_snr";
        case GEOPHONE_PICK_REJECTED_INCONSISTENT:
            return "rejected_inconsistent";
        default:
            return "rejected_no_trigger";
    }
}


static bool pick_status_from_name(
    const char *name,
    geophone_pick_status_t *status
)
{
    if (name == NULL || status == NULL) {
        return false;
    }
    if (strcmp(name, "accepted") == 0) {
        *status = GEOPHONE_PICK_ACCEPTED;
        return true;
    }
    if (strcmp(name, "rejected_no_trigger") == 0) {
        *status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
        return true;
    }
    if (strcmp(name, "rejected_no_refinement") == 0) {
        *status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
        return true;
    }
    if (strcmp(name, "rejected_low_snr") == 0) {
        *status = GEOPHONE_PICK_REJECTED_LOW_SNR;
        return true;
    }
    if (strcmp(name, "rejected_inconsistent") == 0) {
        *status = GEOPHONE_PICK_REJECTED_INCONSISTENT;
        return true;
    }
    return false;
}


static const char *pick_method_name(
    geophone_pick_method_t method
)
{
    switch (method) {
        case GEOPHONE_PICK_METHOD_STA_LTA_REFINED:
        default:
            return "sta_lta_refined";
    }
}


static bool pick_method_from_name(
    const char *name,
    geophone_pick_method_t *method
)
{
    if (name == NULL || method == NULL) {
        return false;
    }
    if (strcmp(name, "sta_lta_refined") == 0) {
        *method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
        return true;
    }
    return false;
}


esp_err_t localization_pick_record_to_json(
    const localization_pick_record_t *record,
    char **json_out
)
{
    if (record == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const geophone_channel_pick_t *pick = &record->channels[i].pick;
        if (
            pick->status == GEOPHONE_PICK_ACCEPTED &&
            (!isfinite(pick->arrival_time_s) || !isfinite(pick->snr))
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ok =
        cJSON_AddNumberToObject(
            root, "schema_version", LOCALIZATION_PICK_RECORD_SCHEMA_VERSION
        ) != NULL &&
        cJSON_AddStringToObject(root, "object_id", record->object_id) != NULL &&
        cJSON_AddStringToObject(root, "session_id", record->session_id) != NULL &&
        cJSON_AddNumberToObject(
            root, "computed_at_ms", (double)record->computed_at_ms
        ) != NULL;

    cJSON *channels = ok ? cJSON_AddArrayToObject(root, "channels") : NULL;
    ok = ok && channels != NULL;

    for (size_t i = 0U; ok && i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const localization_pick_channel_t *channel = &record->channels[i];

        cJSON *entry = cJSON_CreateObject();
        ok = entry != NULL;
        if (ok) {
            cJSON_AddItemToArray(channels, entry);
            ok =
                cJSON_AddNumberToObject(entry, "channel", (double)(i + 1U)) != NULL &&
                cJSON_AddBoolToObject(entry, "participating", channel->participating) != NULL &&
                cJSON_AddStringToObject(
                    entry, "status", pick_status_name(channel->pick.status)
                ) != NULL &&
                cJSON_AddStringToObject(
                    entry, "method", pick_method_name(channel->pick.method)
                ) != NULL &&
                cJSON_AddNumberToObject(
                    entry, "arrival_sample_index", (double)channel->pick.arrival_sample_index
                ) != NULL &&
                cJSON_AddNumberToObject(
                    entry, "arrival_subsample_offset", (double)channel->pick.arrival_subsample_offset
                ) != NULL &&
                cJSON_AddNumberToObject(
                    entry, "arrival_time_s", (double)channel->pick.arrival_time_s
                ) != NULL &&
                cJSON_AddNumberToObject(entry, "snr", (double)channel->pick.snr) != NULL &&
                cJSON_AddNumberToObject(
                    entry, "uncertainty_samples", (double)channel->pick.uncertainty_samples
                ) != NULL;
        }
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


esp_err_t localization_pick_record_from_json(
    const char *json,
    localization_pick_record_t *record
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
    if ((uint32_t)schema_version != LOCALIZATION_PICK_RECORD_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    record->schema_version = (uint32_t)schema_version;

    double computed_at_ms = 0.0;
    bool ok =
        read_string(root, "object_id", record->object_id, sizeof(record->object_id)) &&
        read_string(root, "session_id", record->session_id, sizeof(record->session_id)) &&
        read_number(root, "computed_at_ms", &computed_at_ms) &&
        computed_at_ms >= 0.0;
    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    record->computed_at_ms = (uint64_t)computed_at_ms;

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (!cJSON_IsArray(channels)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(channels, (int)i);
        char status_buffer[32] = { 0 };
        char method_buffer[32] = { 0 };
        double arrival_sample_index = 0.0;
        double arrival_time_s = 0.0;
        double snr = 0.0;
        double uncertainty_samples = 0.0;
        bool participating = false;

        ok = cJSON_IsObject(entry) &&
            read_bool(entry, "participating", &participating) &&
            read_string(entry, "status", status_buffer, sizeof(status_buffer)) &&
            pick_status_from_name(status_buffer, &record->channels[i].pick.status) &&
            read_string(entry, "method", method_buffer, sizeof(method_buffer)) &&
            pick_method_from_name(method_buffer, &record->channels[i].pick.method) &&
            read_number(entry, "arrival_sample_index", &arrival_sample_index) &&
            arrival_sample_index >= 0.0 &&
            read_number(entry, "arrival_time_s", &arrival_time_s) &&
            read_number(entry, "snr", &snr) &&
            read_number(entry, "uncertainty_samples", &uncertainty_samples) &&
            uncertainty_samples >= 0.0;

        if (!ok) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        record->channels[i].participating = participating;
        record->channels[i].pick.arrival_sample_index = (size_t)arrival_sample_index;
        record->channels[i].pick.arrival_time_s = (float)arrival_time_s;
        /* Optional (MAX-209): absent in records written before the field. */
        double subsample_offset = 0.0;
        if (
            !read_number(entry, "arrival_subsample_offset", &subsample_offset) ||
            subsample_offset <= -1.0 ||
            subsample_offset > 0.0
        ) {
            subsample_offset = 0.0;
        }
        record->channels[i].pick.arrival_subsample_offset = (float)subsample_offset;
        record->channels[i].pick.snr = (float)snr;
        record->channels[i].pick.uncertainty_samples = (size_t)uncertainty_samples;
    }

    cJSON_Delete(root);
    return ESP_OK;
}
