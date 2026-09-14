#include "object_store_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "object_format.h"
#include "storage_manager.h"


static bool object_store_fs_entry_is_directory(
    const struct dirent *entry
)
{
    if (entry == NULL) {
        return false;
    }

#if defined(DT_DIR) && defined(DT_UNKNOWN)
    if (entry->d_type == DT_DIR) {
        return true;
    }
    if (entry->d_type != DT_UNKNOWN) {
        return false;
    }
#elif defined(DT_DIR)
    if (entry->d_type == DT_DIR) {
        return true;
    }
#endif

    /*
     * Some libc/FAT dirent variants do not expose d_type (and some return an
     * unknown type). A regular file named like OBJ_9999 must not reserve that
     * object number, so verify only canonical-ID candidates with stat().
     * next_id() is a create-time path, not the hot Web status census.
     */
    char path[OBJECT_STORE_FS_PATH_SIZE];
    const int written = snprintf(
        path,
        sizeof(path),
        "%s/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        entry->d_name
    );
    if (written < 0 || written >= (int)sizeof(path)) {
        return false;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        return false;
    }

#ifdef _WIN32
    return (info.st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(info.st_mode);
#endif
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
                readdir(
                    directory
                )
        ) != NULL
    ) {
        if (
            !object_format_id_is_valid(
                entry->d_name
            )
        ) {
            continue;
        }

        if (!object_store_fs_entry_is_directory(entry)) {
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
            number >
                max_number
        ) {
            max_number =
                number;
        }
    }

    closedir(
        directory
    );

    if (max_number >= 9999) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written =
        snprintf(
            object_id,
            object_id_size,
            "OBJ_%04u",
            max_number + 1
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
