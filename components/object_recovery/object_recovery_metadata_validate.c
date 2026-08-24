#include "object_recovery_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

#include "object_format.h"


bool object_recovery_fs_metadata_valid(
    const char *path,
    const char *expected_id
)
{
    struct stat info;

    if (
        stat(
            path,
            &info
        ) != 0 ||
        !S_ISREG(
            info.st_mode
        ) ||
        info.st_size <= 0 ||
        info.st_size >
            OBJECT_RECOVERY_METADATA_MAX_SIZE
    ) {
        return false;
    }

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (file == NULL) {
        return false;
    }

    size_t size =
        (size_t)
            info.st_size;

    char *buffer =
        malloc(
            size + 1
        );

    if (buffer == NULL) {
        fclose(
            file
        );

        return false;
    }

    size_t read_count =
        fread(
            buffer,
            1,
            size,
            file
        );

    fclose(
        file
    );

    if (read_count != size) {
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

    if (root == NULL) {
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

    bool valid =
        cJSON_IsNumber(
            schema_version
        ) &&
        object_format_schema_is_supported(
            schema_version->valueint
        ) &&
        cJSON_IsString(
            id
        ) &&
        id->valuestring != NULL &&
        object_format_id_is_valid(
            id->valuestring
        ) &&
        strcmp(
            id->valuestring,
            expected_id
        ) == 0;

    cJSON_Delete(
        root
    );

    return valid;
}
