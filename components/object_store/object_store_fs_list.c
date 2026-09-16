#include "object_store_internal.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include "object_format.h"
#include "object_store_archive_policy.h"
#include "storage_manager.h"


static bool object_store_fs_list_entry_is_directory(
    const struct dirent *entry
)
{
    if (entry == NULL) {
        return false;
    }

#if defined(DT_DIR)
    if (entry->d_type == DT_DIR) {
        return true;
    }
#if defined(DT_UNKNOWN)
    if (entry->d_type != DT_UNKNOWN) {
        return false;
    }
#else
    return false;
#endif
#endif

    /* FAT/newlib may report DT_UNKNOWN. Resolve only that ambiguous case so a
     * regular file named OBJ_#### can never consume bounded archive capacity. */
    char path[OBJECT_STORE_FS_PATH_SIZE];
    if (
        object_store_fs_object_path(
            entry->d_name,
            path,
            sizeof(path)
        ) != ESP_OK
    ) {
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


esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id
)
{
    if (objects == NULL || object_count == NULL || max_objects == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0U;
    DIR *directory = opendir(STORAGE_MANAGER_OBJECTS_PATH);
    if (directory == NULL) {
        return ESP_FAIL;
    }

    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS
        ? max_objects
        : OBJECT_STORE_MAX_OBJECTS;
    object_store_archive_selection_t selection;
    object_store_archive_selection_init(
        &selection,
        bounded,
        active_object_id
    );

    /* readdir() order is not an application ordering contract. Scan the whole
     * parent directory while retaining only O(OBJECT_STORE_MAX_OBJECTS) IDs. */
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!object_format_id_is_valid(entry->d_name)) {
            continue;
        }
        if (!object_store_fs_list_entry_is_directory(entry)) {
            continue;
        }

        object_store_archive_selection_consider(
            &selection,
            entry->d_name
        );
    }

    closedir(directory);
    object_store_archive_selection_finalize(&selection);

    const size_t count =
        object_store_archive_selection_count(&selection);
    for (size_t i = 0U; i < count; ++i) {
        const char *object_id =
            object_store_archive_selection_id(&selection, i);
        if (object_id == NULL) {
            continue;
        }

        object_store_summary_t *item = &objects[*object_count];
        object_store_fs_copy_id(
            item->id,
            sizeof(item->id),
            object_id
        );
        object_store_fs_copy_id(
            item->name,
            sizeof(item->name),
            object_id
        );
        item->active = object_store_archive_selection_is_active(
            &selection,
            object_id
        );
        ++(*object_count);
    }

    /* Keep the historic ascending TFT presentation. Membership, not readdir
     * order, now selects newest/top-N plus a pinned existing ACTIVE object. */
    return ESP_OK;
}
