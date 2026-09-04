#include "object_store_internal.h"

#include <dirent.h>
#include <string.h>

#include "object_format.h"
#include "storage_manager.h"

static void object_store_fs_sort(object_store_summary_t *objects, size_t object_count)
{
    for (size_t i = 0; i < object_count; ++i) {
        for (size_t j = i + 1; j < object_count; ++j) {
            if (strcmp(objects[i].id, objects[j].id) <= 0) continue;
            object_store_summary_t temp = objects[i];
            objects[i] = objects[j];
            objects[j] = temp;
        }
    }
}

esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id
)
{
    if (objects == NULL || object_count == NULL || max_objects == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0;
    DIR *directory = opendir(STORAGE_MANAGER_OBJECTS_PATH);
    if (directory == NULL) return ESP_FAIL;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (*object_count >= max_objects) break;
        if (!object_format_id_is_valid(entry->d_name)) continue;

        /*
         * Keep this as a cheap directory census. The old implementation did
         * stat(object.json) for every ID, adding about 100 ms/object before any
         * profile was read. Recovery/profile parsing remains authoritative for
         * metadata validity. DT_UNKNOWN is accepted for FAT implementations
         * that do not populate d_type.
         */
        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;

        object_store_summary_t *item = &objects[*object_count];
        object_store_fs_copy_id(item->id, sizeof(item->id), entry->d_name);
        object_store_fs_copy_id(item->name, sizeof(item->name), entry->d_name);
        item->active = active_object_id != NULL && strcmp(active_object_id, entry->d_name) == 0;
        (*object_count)++;
    }

    closedir(directory);
    object_store_fs_sort(objects, *object_count);
    return ESP_OK;
}
