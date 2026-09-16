#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"


esp_err_t web_status_json_add_objects(cJSON *root);
esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id);


/*
 * Changing ACTIVE is membership-neutral only when the currently publishable
 * payload already contains the target object. Otherwise the bounded snapshot
 * cannot prove that it represents the canonical "newest N + pinned ACTIVE"
 * membership and must be rebuilt before it is served again.
 */
static inline bool web_status_active_membership_requires_refresh(
    bool cache_ready,
    bool cache_refreshing,
    bool payload_available,
    bool object_present
)
{
    return
        !cache_ready ||
        cache_refreshing ||
        !payload_available ||
        !object_present;
}


/*
 * Append optional cache-backed sections while preserving the exact reason a
 * locked cache read failed. Cache invalidation/refresh is a normal transient
 * state (ESP_ERR_INVALID_STATE); JSON allocation failure remains NO_MEM.
 */
static inline esp_err_t web_status_json_add_cache_sections(
    cJSON *root,
    bool include_objects,
    const char *sessions_object_id
)
{
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (include_objects) {
        const esp_err_t err = web_status_json_add_objects(root);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (sessions_object_id != NULL && sessions_object_id[0] != '\0') {
        const esp_err_t err = web_status_json_add_sessions(
            root,
            sessions_object_id
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}
