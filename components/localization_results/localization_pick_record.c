#include "localization_pick_record.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "json_number.h"
#include "localization_results_json_int64.h"


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
        case GEOPHONE_PICK_REJECTED_SATURATED:
            return "rejected_saturated";
        case GEOPHONE_PICK_REJECTED_AMBIGUOUS:
            return "rejected_ambiguous";
        default:
            return NULL;
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
    if (strcmp(name, "rejected_saturated") == 0) {
        *status = GEOPHONE_PICK_REJECTED_SATURATED;
        return true;
    }
    if (strcmp(name, "rejected_ambiguous") == 0) {
        *status = GEOPHONE_PICK_REJECTED_AMBIGUOUS;
        return true;
    }
    return false;
}


static const char *pick_reason_name(geophone_pick_reason_t reason)
{
    switch (reason) {
        case GEOPHONE_PICK_REASON_NONE: return "";
        case GEOPHONE_PICK_REASON_SATURATED: return "saturated";
        case GEOPHONE_PICK_REASON_AMBIGUOUS: return "ambiguous";
        default: return NULL;
    }
}


static const char *pick_record_reason(const localization_pick_channel_t *channel)
{
    if (strnlen(channel->reason, sizeof(channel->reason)) >= sizeof(channel->reason)) {
        return NULL;
    }
    if (channel->reason[0] != '\0') {
        return channel->reason;
    }
    const char *reason = pick_reason_name(channel->pick.reason);
    if (reason == NULL) {
        return NULL;
    }
    if (reason[0] != '\0') {
        return reason;
    }
    if (channel->pick.status == GEOPHONE_PICK_REJECTED_SATURATED) {
        return "saturated";
    }
    if (channel->pick.status == GEOPHONE_PICK_REJECTED_AMBIGUOUS) {
        return "ambiguous";
    }
    return "";
}


static const char *pick_method_name(
    geophone_pick_method_t method
)
{
    switch (method) {
        case GEOPHONE_PICK_METHOD_STA_LTA_REFINED:
            return "sta_lta_refined";
        default:
            return NULL;
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
        const localization_pick_channel_t *channel = &record->channels[i];
        const geophone_channel_pick_t *pick = &channel->pick;
        const char *status = pick_status_name(pick->status);
        const char *method = pick_method_name(pick->method);
        const char *reason = pick_record_reason(channel);
        if (status == NULL || method == NULL || reason == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (
            pick->status == GEOPHONE_PICK_ACCEPTED &&
            (!isfinite(pick->arrival_time_s) || !isfinite(pick->snr))
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        if (channel->s_pick.present) {
            const localization_pick_phase_t *phase = &channel->s_pick;
            if (
                (phase->fields_present & LOCALIZATION_PICK_PHASE_FIELDS_ALL) !=
                    phase->fields_present ||
                strnlen(phase->reason, sizeof(phase->reason)) >= sizeof(phase->reason) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_STATUS) != 0U &&
                 pick_status_name(phase->status) == NULL) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_METHOD) != 0U &&
                 pick_method_name(phase->method) == NULL) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_SUBSAMPLE_OFFSET) != 0U &&
                 (!isfinite(phase->subsample_offset) || phase->subsample_offset <= -1.0f ||
                  phase->subsample_offset > 0.0f)) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_ARRIVAL_TIME_US) != 0U &&
                 !isfinite(phase->arrival_time_us)) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_SNR) != 0U &&
                 !isfinite(phase->snr)) ||
                ((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_UNCERTAINTY_US) != 0U &&
                 (!isfinite(phase->uncertainty_us) || phase->uncertainty_us < 0.0))
            ) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (
            channel->s_pick.fields_present != 0U || channel->s_pick.reason[0] != '\0'
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bool ok = cJSON_AddNumberToObject(
        root, "schema_version", LOCALIZATION_PICK_RECORD_SCHEMA_VERSION
    ) != NULL;
    ok = ok &&
        cJSON_AddBoolToObject(root, "session_epoch_known", record->session_epoch_known) != NULL &&
        (record->session_epoch_known
            ? cJSON_AddNumberToObject(
                root, "session_epoch_timestamp_us",
                (double)record->session_epoch_timestamp_us
            ) != NULL
            : cJSON_AddNullToObject(root, "session_epoch_timestamp_us") != NULL) &&
        cJSON_AddStringToObject(root, "object_id", record->object_id) != NULL &&
        cJSON_AddStringToObject(root, "session_id", record->session_id) != NULL &&
        cJSON_AddNumberToObject(
            root, "computed_at_ms", (double)record->computed_at_ms
        ) != NULL;

    cJSON *channels = ok ? cJSON_AddArrayToObject(root, "channels") : NULL;
    ok = ok && channels != NULL;

    for (size_t i = 0U; ok && i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
        const localization_pick_channel_t *channel = &record->channels[i];
        const char *reason = pick_record_reason(channel);

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
            if (ok && reason != NULL && reason[0] != '\0') {
                ok = cJSON_AddStringToObject(entry, "reason", reason) != NULL;
            }
            if (ok && channel->s_pick.present) {
                cJSON *s_pick = cJSON_AddObjectToObject(entry, "s_pick");
                ok = s_pick != NULL;
                const uint8_t fields_present = channel->s_pick.fields_present;
                if (ok && (fields_present & LOCALIZATION_PICK_PHASE_FIELD_STATUS) != 0U) {
                    ok = cJSON_AddStringToObject(
                        s_pick, "status", pick_status_name(channel->s_pick.status)
                    ) != NULL;
                }
                if (ok && channel->s_pick.reason[0] != '\0') {
                    ok = cJSON_AddStringToObject(
                        s_pick, "reason", channel->s_pick.reason
                    ) != NULL;
                }
                if (ok && (fields_present & LOCALIZATION_PICK_PHASE_FIELD_METHOD) != 0U) {
                    ok = cJSON_AddStringToObject(
                        s_pick, "method", pick_method_name(channel->s_pick.method)
                    ) != NULL;
                }
                if (ok && (fields_present & LOCALIZATION_PICK_PHASE_FIELD_SAMPLE_INDEX) != 0U) {
                    ok = cJSON_AddNumberToObject(
                        s_pick, "sample_index", (double)channel->s_pick.sample_index
                    ) != NULL;
                }
                if (ok &&
                    (fields_present & LOCALIZATION_PICK_PHASE_FIELD_SUBSAMPLE_OFFSET) != 0U) {
                    ok = cJSON_AddNumberToObject(
                        s_pick, "subsample_offset", (double)channel->s_pick.subsample_offset
                    ) != NULL;
                }
                if (ok &&
                    (fields_present & LOCALIZATION_PICK_PHASE_FIELD_ARRIVAL_TIME_US) != 0U) {
                    ok = cJSON_AddNumberToObject(
                        s_pick, "arrival_time_us", channel->s_pick.arrival_time_us
                    ) != NULL;
                }
                if (ok && (fields_present & LOCALIZATION_PICK_PHASE_FIELD_SNR) != 0U) {
                    ok = cJSON_AddNumberToObject(s_pick, "snr", channel->s_pick.snr) != NULL;
                }
                if (ok &&
                    (fields_present & LOCALIZATION_PICK_PHASE_FIELD_UNCERTAINTY_US) != 0U) {
                    ok = cJSON_AddNumberToObject(
                        s_pick, "uncertainty_us", channel->s_pick.uncertainty_us
                    ) != NULL;
                }
            }
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
    if (
        record->session_epoch_known &&
        !localization_results_json_replace_int64(
            &json, "session_epoch_timestamp_us", record->session_epoch_timestamp_us
        )
    ) {
        free(json);
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
    uint32_t schema_number = 0U;
    if (
        !read_number(root, "schema_version", &schema_version) ||
        !json_number_to_uint32(schema_version, &schema_number)
    ) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (schema_number != LOCALIZATION_PICK_RECORD_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    record->schema_version = schema_number;

    double computed_at_ms = 0.0;
    uint64_t computed_at = 0U;
    bool ok =
        read_string(root, "object_id", record->object_id, sizeof(record->object_id)) &&
        read_string(root, "session_id", record->session_id, sizeof(record->session_id)) &&
        read_number(root, "computed_at_ms", &computed_at_ms) &&
        json_number_to_uint64(computed_at_ms, &computed_at);
    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    record->computed_at_ms = computed_at;

    const cJSON *epoch_known_item = cJSON_GetObjectItemCaseSensitive(
        root, "session_epoch_known"
    );
    const cJSON *epoch_timestamp_item = cJSON_GetObjectItemCaseSensitive(
        root, "session_epoch_timestamp_us"
    );
    if (epoch_known_item != NULL || epoch_timestamp_item != NULL) {
        bool epoch_known = false;
        int64_t epoch_timestamp_us = 0;
        bool epoch_timestamp_present = false;
        bool epoch_timestamp_is_null = false;
        if (
            !read_bool(root, "session_epoch_known", &epoch_known) ||
            !localization_results_json_read_int64(
                json, "session_epoch_timestamp_us", &epoch_timestamp_us,
                &epoch_timestamp_present, &epoch_timestamp_is_null
            )
        ) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (epoch_known) {
            if (
                !cJSON_IsNumber(epoch_timestamp_item) ||
                !epoch_timestamp_present || epoch_timestamp_is_null
            ) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
            record->session_epoch_known = true;
            record->session_epoch_timestamp_us = epoch_timestamp_us;
        } else if (
            (epoch_timestamp_present && !epoch_timestamp_is_null) ||
            (epoch_timestamp_item != NULL && !cJSON_IsNull(epoch_timestamp_item))
        ) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

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
        size_t arrival_index = 0U;
        size_t uncertainty_count = 0U;
        bool participating = false;

        ok = cJSON_IsObject(entry) &&
            read_bool(entry, "participating", &participating) &&
            read_string(entry, "status", status_buffer, sizeof(status_buffer)) &&
            pick_status_from_name(status_buffer, &record->channels[i].pick.status) &&
            read_string(entry, "method", method_buffer, sizeof(method_buffer)) &&
            pick_method_from_name(method_buffer, &record->channels[i].pick.method) &&
            read_number(entry, "arrival_sample_index", &arrival_sample_index) &&
            json_number_to_size(arrival_sample_index, &arrival_index) &&
            read_number(entry, "arrival_time_s", &arrival_time_s) &&
            read_number(entry, "snr", &snr) &&
            read_number(entry, "uncertainty_samples", &uncertainty_samples) &&
            json_number_to_size(uncertainty_samples, &uncertainty_count);

        if (!ok) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        record->channels[i].participating = participating;
        record->channels[i].pick.arrival_sample_index = arrival_index;
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
        record->channels[i].pick.uncertainty_samples = uncertainty_count;

        const cJSON *reason_item = cJSON_GetObjectItemCaseSensitive(entry, "reason");
        if (reason_item != NULL) {
            if (!read_string(
                entry, "reason", record->channels[i].reason,
                sizeof(record->channels[i].reason)
            )) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (strcmp(record->channels[i].reason, "saturated") == 0) {
                record->channels[i].pick.reason = GEOPHONE_PICK_REASON_SATURATED;
            } else if (strcmp(record->channels[i].reason, "ambiguous") == 0) {
                record->channels[i].pick.reason = GEOPHONE_PICK_REASON_AMBIGUOUS;
            }
        }

        const cJSON *s_pick = cJSON_GetObjectItemCaseSensitive(entry, "s_pick");
        if (s_pick != NULL) {
            localization_pick_phase_t *phase = &record->channels[i].s_pick;
            char s_status_buffer[32] = { 0 };
            char s_method_buffer[32] = { 0 };
            double sample_index = 0.0;
            double s_subsample_offset = 0.0;
            double arrival_time_us = 0.0;
            double s_snr = 0.0;
            double uncertainty_us = 0.0;
            ok = cJSON_IsObject(s_pick);
            phase->present = ok;
            const cJSON *s_status_item = cJSON_GetObjectItemCaseSensitive(s_pick, "status");
            if (ok && s_status_item != NULL) {
                ok = read_string(s_pick, "status", s_status_buffer, sizeof(s_status_buffer)) &&
                    pick_status_from_name(s_status_buffer, &phase->status);
                if (ok) phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_STATUS;
            }
            const cJSON *s_reason_item = cJSON_GetObjectItemCaseSensitive(s_pick, "reason");
            if (ok && s_reason_item != NULL) {
                ok = read_string(s_pick, "reason", phase->reason, sizeof(phase->reason));
            }
            const cJSON *s_method_item = cJSON_GetObjectItemCaseSensitive(s_pick, "method");
            if (ok && s_method_item != NULL) {
                ok = read_string(s_pick, "method", s_method_buffer, sizeof(s_method_buffer)) &&
                    pick_method_from_name(s_method_buffer, &phase->method);
                if (ok) phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_METHOD;
            }
            const cJSON *sample_index_item = cJSON_GetObjectItemCaseSensitive(
                s_pick, "sample_index"
            );
            if (ok && sample_index_item != NULL) {
                ok = read_number(s_pick, "sample_index", &sample_index) &&
                    sample_index >= 0.0 && floor(sample_index) == sample_index &&
                    sample_index <= 9007199254740991.0 &&
                    sample_index <= (double)SIZE_MAX;
                if (ok) {
                    phase->sample_index = (size_t)sample_index;
                    phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_SAMPLE_INDEX;
                }
            }
            const cJSON *subsample_offset_item = cJSON_GetObjectItemCaseSensitive(
                s_pick, "subsample_offset"
            );
            if (ok && subsample_offset_item != NULL) {
                ok = read_number(s_pick, "subsample_offset", &s_subsample_offset) &&
                    s_subsample_offset > -1.0 && s_subsample_offset <= 0.0;
                if (ok) {
                    phase->subsample_offset = (float)s_subsample_offset;
                    phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_SUBSAMPLE_OFFSET;
                }
            }
            const cJSON *arrival_time_item = cJSON_GetObjectItemCaseSensitive(
                s_pick, "arrival_time_us"
            );
            if (ok && arrival_time_item != NULL) {
                ok = read_number(s_pick, "arrival_time_us", &arrival_time_us);
                if (ok) {
                    phase->arrival_time_us = arrival_time_us;
                    phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_ARRIVAL_TIME_US;
                }
            }
            const cJSON *snr_item = cJSON_GetObjectItemCaseSensitive(s_pick, "snr");
            if (ok && snr_item != NULL) {
                ok = read_number(s_pick, "snr", &s_snr);
                if (ok) {
                    phase->snr = s_snr;
                    phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_SNR;
                }
            }
            const cJSON *uncertainty_item = cJSON_GetObjectItemCaseSensitive(
                s_pick, "uncertainty_us"
            );
            if (ok && uncertainty_item != NULL) {
                ok = read_number(s_pick, "uncertainty_us", &uncertainty_us) &&
                    uncertainty_us >= 0.0;
                if (ok) {
                    phase->uncertainty_us = uncertainty_us;
                    phase->fields_present |= LOCALIZATION_PICK_PHASE_FIELD_UNCERTAINTY_US;
                }
            }
            if (!ok) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
