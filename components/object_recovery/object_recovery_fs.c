#include "object_recovery_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"


#define RECOVERY_PATH_SIZE          256
#define RECOVERY_METADATA_MAX_SIZE 16384


static esp_err_t object_recovery_build_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *file_name
)
{
    if (
        buffer == NULL ||
        directory == NULL ||
        file_name == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    const int written =
        snprintf(
            buffer,
            buffer_size,
            "%s/%s",
            directory,
            file_name
        );


    if (
        written < 0 ||
        (size_t)written >= buffer_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return ESP_OK;
}


static bool object_recovery_metadata_valid(
    const char *path,
    const char *expected_id
)
{
    struct stat info;


    if (
        stat(
            path,
            &info
        ) != 0
    ) {
        return false;
    }


    if (
        !S_ISREG(
            info.st_mode
        ) ||
        info.st_size <= 0 ||
        info.st_size >
            RECOVERY_METADATA_MAX_SIZE
    ) {
        return false;
    }


    FILE *file =
        fopen(
            path,
            "rb"
        );


    if (
        file ==
        NULL
    ) {
        return false;
    }


    const size_t size =
        (size_t)info.st_size;


    char *buffer =
        malloc(
            size + 1
        );


    if (
        buffer ==
        NULL
    ) {
        fclose(
            file
        );


        return false;
    }


    const size_t read_count =
        fread(
            buffer,
            1,
            size,
            file
        );


    fclose(
        file
    );


    if (
        read_count !=
        size
    ) {
        free(
            buffer
        );


        return false;
    }


    buffer[size] =
        '\0';


    cJSON *root =
        cJSON_Parse(
            buffer
        );


    free(
        buffer
    );


    if (
        root ==
        NULL
    ) {
        return false;
    }


    const cJSON *schema_version =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "schema_version"
        );


    const cJSON *id =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "id"
        );


    const bool valid =
        cJSON_IsNumber(
            schema_version
        ) &&
        schema_version->valueint >= 1 &&
        cJSON_IsString(
            id
        ) &&
        id->valuestring != NULL &&
        strcmp(
            id->valuestring,
            expected_id
        ) == 0;


    cJSON_Delete(
        root
    );


    return valid;
}


static esp_err_t object_recovery_unlink_if_exists(
    const char *path
)
{
    if (
        unlink(
            path
        ) == 0
    ) {
        return ESP_OK;
    }


    if (
        errno ==
        ENOENT
    ) {
        return ESP_OK;
    }


    return ESP_FAIL;
}


static esp_err_t object_recovery_restore_file(
    const char *source,
    const char *destination
)
{
    esp_err_t err =
        object_recovery_unlink_if_exists(
            destination
        );


    if (
        err !=
        ESP_OK
    ) {
        return err;
    }


    if (
        rename(
            source,
            destination
        ) != 0
    ) {
        return ESP_FAIL;
    }


    return ESP_OK;
}


esp_err_t object_recovery_fs_prepare_metadata(
    const char *object_directory,
    const char *object_id,
    bool *valid,
    bool *recovered
)
{
    if (
        object_directory == NULL ||
        object_id == NULL ||
        valid == NULL ||
        recovered == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    *valid =
        false;


    *recovered =
        false;


    char primary[
        RECOVERY_PATH_SIZE
    ];


    char backup[
        RECOVERY_PATH_SIZE
    ];


    char temporary[
        RECOVERY_PATH_SIZE
    ];


    esp_err_t err =
        object_recovery_build_path(
            primary,
            sizeof(primary),
            object_directory,
            "object.json"
        );


    if (
        err !=
        ESP_OK
    ) {
        return err;
    }


    err =
        object_recovery_build_path(
            backup,
            sizeof(backup),
            object_directory,
            "object.json.bak"
        );


    if (
        err !=
        ESP_OK
    ) {
        return err;
    }


    err =
        object_recovery_build_path(
            temporary,
            sizeof(temporary),
            object_directory,
            "object.json.tmp"
        );


    if (
        err !=
        ESP_OK
    ) {
        return err;
    }


    if (
        object_recovery_metadata_valid(
            primary,
            object_id
        )
    ) {
        object_recovery_unlink_if_exists(
            temporary
        );


        *valid =
            true;


        return ESP_OK;
    }


    if (
        object_recovery_metadata_valid(
            backup,
            object_id
        )
    ) {
        err =
            object_recovery_restore_file(
                backup,
                primary
            );


        if (
            err !=
            ESP_OK
        ) {
            return err;
        }


        object_recovery_unlink_if_exists(
            temporary
        );


        *valid =
            true;


        *recovered =
            true;


        return ESP_OK;
    }


    if (
        object_recovery_metadata_valid(
            temporary,
            object_id
        )
    ) {
        err =
            object_recovery_restore_file(
                temporary,
                primary
            );


        if (
            err !=
            ESP_OK
        ) {
            return err;
        }


        object_recovery_unlink_if_exists(
            backup
        );


        *valid =
            true;


        *recovered =
            true;


        return ESP_OK;
    }


    return ESP_OK;
}


esp_err_t object_recovery_fs_remove_tree(
    const char *path
)
{
    if (
        path ==
        NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    struct stat info;


    if (
        stat(
            path,
            &info
        ) != 0
    ) {
        return
            errno == ENOENT
                ? ESP_OK
                : ESP_FAIL;
    }


    if (
        !S_ISDIR(
            info.st_mode
        )
    ) {
        return
            unlink(path) == 0
                ? ESP_OK
                : ESP_FAIL;
    }


    DIR *directory =
        opendir(
            path
        );


    if (
        directory ==
        NULL
    ) {
        return ESP_FAIL;
    }


    esp_err_t result =
        ESP_OK;


    struct dirent *entry;


    while (
        (
            entry =
                readdir(
                    directory
                )
        ) != NULL
    ) {
        if (
            strcmp(
                entry->d_name,
                "."
            ) == 0 ||
            strcmp(
                entry->d_name,
                ".."
            ) == 0
        ) {
            continue;
        }


        char child[
            RECOVERY_PATH_SIZE
        ];


        const int written =
            snprintf(
                child,
                sizeof(child),
                "%s/%s",
                path,
                entry->d_name
            );


        if (
            written < 0 ||
            (size_t)written >=
                sizeof(child)
        ) {
            result =
                ESP_ERR_INVALID_SIZE;


            break;
        }


        result =
            object_recovery_fs_remove_tree(
                child
            );


        if (
            result !=
            ESP_OK
        ) {
            break;
        }
    }


    closedir(
        directory
    );


    if (
        result !=
        ESP_OK
    ) {
        return result;
    }


    if (
        rmdir(
            path
        ) != 0
    ) {
        return ESP_FAIL;
    }


    return ESP_OK;
}