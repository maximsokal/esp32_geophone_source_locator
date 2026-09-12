#include "object_store.h"

#include <stdlib.h>
#include <string.h>

#include "object_store_internal.h"


esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    if (
        objects == NULL ||
        object_count == NULL ||
        default_cable_lengths_m == NULL ||
        max_objects == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0U;

    const size_t bounded =
        max_objects < OBJECT_STORE_MAX_OBJECTS
            ? max_objects
            : OBJECT_STORE_MAX_OBJECTS;

    object_store_summary_t *summaries =
        calloc(bounded, sizeof(*summaries));
    if (summaries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The normal profile-index cold build intentionally holds one high-level
     * storage transaction across every object.json. That is appropriate for a
     * coherent cache rebuild but it can keep the shared TFT/SD SPI guard for
     * seconds. The Web status path instead takes one cheap directory census,
     * then releases the guard between profile files. The object-store mutex is
     * reacquired in the normal storage->object order for every bounded read.
     */
    esp_err_t err =
        object_store_internal_begin_file_operation();
    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    size_t summary_count = 0U;
    err = object_store_fs_list(
        summaries,
        bounded,
        &summary_count,
        object_store_internal_active_id()
    );

    object_store_internal_end_file_operation();

    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    /* Prime the tiny settings cache once. The legacy index builder treats a
     * missing/unreadable settings file as defaults; preserve that behavior. */
    err = object_store_get_cable_defaults(default_cable_lengths_m);
    if (err != ESP_OK) {
        for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
            default_cable_lengths_m[pod] =
                OBJECT_STORE_DEFAULT_CABLE_LENGTH_M;
        }
    }

    size_t written = 0U;
    for (size_t i = 0U; i < summary_count; ++i) {
        object_store_profile_t profile;
        memset(&profile, 0, sizeof(profile));

        err = object_store_internal_begin_file_operation();
        if (err != ESP_OK) {
            free(summaries);
            return err;
        }

        err = object_store_json_read_profile_locked(
            summaries[i].id,
            summaries[i].active,
            &profile
        );

        object_store_internal_end_file_operation();

        /* A concurrent object deletion after the census is benign for a
         * status snapshot. Other metadata/I/O failures stay authoritative. */
        if (err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            free(summaries);
            return err;
        }

        objects[written++] = profile;
    }

    *object_count = written;
    free(summaries);
    return ESP_OK;
}
