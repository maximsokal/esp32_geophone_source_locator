#include "object_store_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "storage_manager.h"


#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"

#define OBJECT_PATH_SIZE  192

#define OBJECT_ID_TEXT_LENGTH  8


static esp_err_t object_store_fs_object_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    int written =
        snprintf(
            path,
            path_size,
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );


    if (
        written < 0 ||
        written >=
            (int)path_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return ESP_OK;
}


static esp_err_t object_store_fs_metadata_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    int written =
        snprintf(
            path,
            path_size,
            "%s/%s/object.json",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );


    if (
        written < 0 ||
        written >=
            (int)path_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return ESP_OK;
}


static void object_store_fs_copy_id(
    char *destination,
    size_t destination_size,
    const char *object_id
)
{
    if (
        destination == NULL ||
        object_id == NULL ||
        destination_size <=
            OBJECT_ID_TEXT_LENGTH
    ) {
        return;
    }


    memcpy(
        destination,
        object_id,
        OBJECT_ID_TEXT_LENGTH
    );


    destination[
        OBJECT_ID_TEXT_LENGTH
    ] = '\0';
}


bool object_store_fs_id_is_valid(
    const char *object_id
)
{
    if (object_id == NULL) {
        return false;
    }


    if (
        strlen(object_id) !=
        OBJECT_ID_TEXT_LENGTH
    ) {
        return false;
    }


    if (
        strncmp(
            object_id,
            "OBJ_",
            4
        ) != 0
    ) {
        return false;
    }


    for (
        size_t i = 4;
        i < OBJECT_ID_TEXT_LENGTH;
        ++i
    ) {
        if (
            !isdigit(
                (unsigned char)
                    object_id[i]
            )
        ) {
            return false;
        }
    }


    return true;
}


bool object_store_fs_object_exists(
    const char *object_id
)
{
    if (
        !object_store_fs_id_is_valid(
            object_id
        )
    ) {
        return false;
    }


    char metadata_path[
        OBJECT_PATH_SIZE
    ];


    if (
        object_store_fs_metadata_path(
            object_id,
            metadata_path,
            sizeof(metadata_path)
        ) != ESP_OK
    ) {
        return false;
    }


    return storage_manager_path_exists(
        metadata_path
    );
}


esp_err_t object_store_fs_next_id(
    char *object_id,
    size_t object_id_size
)
{
    if (
        object_id == NULL ||
        object_id_size <
            OBJECT_STORE_ID_SIZE
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    DIR *directory =
        opendir(
            STORAGE_MANAGER_OBJECTS_PATH
        );


    if (directory == NULL) {
        return ESP_FAIL;
    }


    unsigned max_number =
        0;


    struct dirent *entry;


    while (
        (
            entry =
                readdir(directory)
        ) != NULL
    ) {
        if (
            !object_store_fs_id_is_valid(
                entry->d_name
            )
        ) {
            continue;
        }


        unsigned number =
            0;


        if (
            sscanf(
                entry->d_name,
                "OBJ_%4u",
                &number
            ) == 1 &&
            number > max_number
        ) {
            max_number =
                number;
        }
    }


    closedir(
        directory
    );


    if (
        max_number >= 9999
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    unsigned next_number =
        max_number + 1;


    int written =
        snprintf(
            object_id,
            object_id_size,
            "OBJ_%04u",
            next_number
        );


    if (
        written < 0 ||
        written >=
            (int)object_id_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return ESP_OK;
}


esp_err_t object_store_fs_create_layout(
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


    char object_path[
        OBJECT_PATH_SIZE
    ];


    esp_err_t err =
        object_store_fs_object_path(
            object_id,
            object_path,
            sizeof(object_path)
        );


    if (err != ESP_OK) {
        return err;
    }


    err =
        storage_manager_create_directory(
            object_path
        );


    if (err != ESP_OK) {
        return err;
    }


    char sessions_path[
        OBJECT_PATH_SIZE
    ];


    int written =
        snprintf(
            sessions_path,
            sizeof(sessions_path),
            "%s/sessions",
            object_path
        );


    if (
        written < 0 ||
        written >=
            (int)sizeof(sessions_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return storage_manager_create_directory(
        sessions_path
    );
}


esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id
)
{
    if (
        objects == NULL ||
        object_count == NULL ||
        max_objects == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    *object_count =
        0;


    DIR *directory =
        opendir(
            STORAGE_MANAGER_OBJECTS_PATH
        );


    if (directory == NULL) {
        return ESP_FAIL;
    }


    struct dirent *entry;


    while (
        (
            entry =
                readdir(directory)
        ) != NULL
    ) {
        if (
            *object_count >=
            max_objects
        ) {
            break;
        }


        if (
            !object_store_fs_id_is_valid(
                entry->d_name
            ) ||
            !object_store_fs_object_exists(
                entry->d_name
            )
        ) {
            continue;
        }


        object_store_summary_t *item =
            &objects[*object_count];


        object_store_fs_copy_id(
            item->id,
            sizeof(item->id),
            entry->d_name
        );


        object_store_fs_copy_id(
            item->name,
            sizeof(item->name),
            entry->d_name
        );


        item->active =
            active_object_id != NULL &&
            strcmp(
                active_object_id,
                entry->d_name
            ) == 0;


        (*object_count)++;
    }


    closedir(
        directory
    );


    for (
        size_t i = 0;
        i < *object_count;
        ++i
    ) {
        for (
            size_t j = i + 1;
            j < *object_count;
            ++j
        ) {
            if (
                strcmp(
                    objects[i].id,
                    objects[j].id
                ) > 0
            ) {
                object_store_summary_t temp =
                    objects[i];


                objects[i] =
                    objects[j];


                objects[j] =
                    temp;
            }
        }
    }


    return ESP_OK;
}


esp_err_t object_store_fs_load_active(
    char *object_id,
    size_t object_id_size
)
{
    if (
        object_id == NULL ||
        object_id_size <
            OBJECT_STORE_ID_SIZE
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    char buffer[64];


    esp_err_t err =
        storage_manager_read_text(
            ACTIVE_OBJECT_PATH,
            buffer,
            sizeof(buffer)
        );


    if (err != ESP_OK) {
        return err;
    }


    buffer[
        strcspn(
            buffer,
            "\r\n\t "
        )
    ] = '\0';


    if (
        !object_store_fs_id_is_valid(
            buffer
        )
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }


    object_store_fs_copy_id(
        object_id,
        object_id_size,
        buffer
    );


    return ESP_OK;
}


esp_err_t object_store_fs_save_active(
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


    char text[
        OBJECT_STORE_ID_SIZE + 2
    ];


    int written =
        snprintf(
            text,
            sizeof(text),
            "%s\n",
            object_id
        );


    if (
        written < 0 ||
        written >=
            (int)sizeof(text)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return storage_manager_atomic_write_text(
        ACTIVE_OBJECT_PATH,
        text
    );
}