#include "object_store_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage_manager.h"


#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"

#define OBJECT_DELETE_PATH_SIZE 256


static esp_err_t object_store_remove_tree(
    const char *path
)
{
    struct stat info;

    if (stat(path, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!S_ISDIR(info.st_mode)) {
        return remove(path) == 0
            ? ESP_OK
            : ESP_FAIL;
    }

    DIR *directory = opendir(path);

    if (directory == NULL) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        if (
            strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0
        ) {
            continue;
        }

        char child_path[OBJECT_DELETE_PATH_SIZE];

        int written = snprintf(
            child_path,
            sizeof(child_path),
            "%s/%s",
            path,
            entry->d_name
        );

        if (
            written < 0 ||
            written >= (int)sizeof(child_path)
        ) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        result = object_store_remove_tree(
            child_path
        );

        if (result != ESP_OK) {
            break;
        }
    }

    closedir(directory);

    if (result != ESP_OK) {
        return result;
    }

    return rmdir(path) == 0
        ? ESP_OK
        : ESP_FAIL;
}


esp_err_t object_store_fs_clear_active(void)
{
    if (
        !storage_manager_path_exists(
            ACTIVE_OBJECT_PATH
        )
    ) {
        return ESP_OK;
    }

    return remove(ACTIVE_OBJECT_PATH) == 0
        ? ESP_OK
        : ESP_FAIL;
}


esp_err_t object_store_fs_delete_object(
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
        OBJECT_DELETE_PATH_SIZE
    ];

    int written = snprintf(
        object_path,
        sizeof(object_path),
        "%s/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id
    );

    if (
        written < 0 ||
        written >= (int)sizeof(object_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return object_store_remove_tree(
        object_path
    );
}