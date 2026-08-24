#include "object_store_internal.h"

#include <stdio.h>

#include "cJSON.h"

#include "storage_manager.h"


#define OBJECT_METADATA_PATH_SIZE  192


esp_err_t object_store_json_write_metadata(
    const char *object_id
)
{
    if (
        !object_store_fs_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    cJSON *root =
        cJSON_CreateObject();


    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }


    cJSON *schema =
        cJSON_AddNumberToObject(
            root,
            "schema_version",
            1
        );


    cJSON *id =
        cJSON_AddStringToObject(
            root,
            "id",
            object_id
        );


    cJSON *name =
        cJSON_AddStringToObject(
            root,
            "name",
            object_id
        );


    cJSON *location =
        cJSON_AddObjectToObject(
            root,
            "location"
        );


    if (
        schema == NULL ||
        id == NULL ||
        name == NULL ||
        location == NULL
    ) {
        cJSON_Delete(
            root
        );

        return ESP_ERR_NO_MEM;
    }


    if (
        cJSON_AddBoolToObject(
            location,
            "valid",
            false
        ) == NULL ||
        cJSON_AddNumberToObject(
            location,
            "latitude",
            0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            location,
            "longitude",
            0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            location,
            "accuracy_m",
            0.0
        ) == NULL ||
        cJSON_AddStringToObject(
            location,
            "source",
            "NONE"
        ) == NULL
    ) {
        cJSON_Delete(
            root
        );

        return ESP_ERR_NO_MEM;
    }


    char *json =
        cJSON_PrintUnformatted(
            root
        );


    cJSON_Delete(
        root
    );


    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }


    char metadata_path[
        OBJECT_METADATA_PATH_SIZE
    ];


    int written =
        snprintf(
            metadata_path,
            sizeof(metadata_path),
            "%s/%s/object.json",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );


    if (
        written < 0 ||
        written >=
            (int)sizeof(metadata_path)
    ) {
        cJSON_free(
            json
        );

        return ESP_ERR_INVALID_SIZE;
    }


    esp_err_t err =
        storage_manager_atomic_write_text(
            metadata_path,
            json
        );


    cJSON_free(
        json
    );


    return err;
}
