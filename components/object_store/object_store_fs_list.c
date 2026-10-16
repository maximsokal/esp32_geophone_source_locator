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


static void object_store_fs_delete_scope_increment(uint32_t *value)
{
    if (*value < UINT32_MAX) ++(*value);
}

static bool object_store_fs_is_directory_mode(const struct stat *info)
{
#ifdef _WIN32
    return (info->st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(info->st_mode);
#endif
}

static bool object_store_fs_is_localization_run_file(const char *name)
{
    static const char prefix[] = "localization_run_";
    const size_t prefix_length = sizeof(prefix) - 1U;
    const size_t name_length = strlen(name);
    return name_length > prefix_length + 5U &&
        strncmp(name, prefix, prefix_length) == 0 &&
        strcmp(name + name_length - 5U, ".json") == 0;
}

static esp_err_t object_store_fs_count_delete_scope_directory(
    const char *path,
    bool object_root,
    bool sessions_parent,
    object_store_delete_scope_t *scope)
{
    DIR *directory = opendir(path);
    if (directory == NULL) return ESP_FAIL;

    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char child_path[OBJECT_STORE_FS_PATH_SIZE];
        const int written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        if (written < 0 || written >= (int)sizeof(child_path)) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        struct stat info;
        if (stat(child_path, &info) != 0) {
            result = ESP_FAIL;
            break;
        }
        if (object_store_fs_is_directory_mode(&info)) {
            if (sessions_parent) object_store_fs_delete_scope_increment(&scope->sessions);
            const bool child_is_sessions_parent = object_root && strcmp(entry->d_name, "sessions") == 0;
            result = object_store_fs_count_delete_scope_directory(
                child_path,
                false,
                child_is_sessions_parent,
                scope
            );
            if (result != ESP_OK) break;
            continue;
        }

        if (info.st_size < 0) {
            result = ESP_FAIL;
            break;
        }
        const uint64_t file_bytes = (uint64_t)info.st_size;
        scope->bytes = file_bytes > UINT64_MAX - scope->bytes
            ? UINT64_MAX
            : scope->bytes + file_bytes;

        if (object_root && strcmp(entry->d_name, "object.json") == 0) continue;
        if (object_store_fs_is_localization_run_file(entry->d_name)) {
            object_store_fs_delete_scope_increment(&scope->localization_runs);
        } else {
            object_store_fs_delete_scope_increment(&scope->artifacts);
        }
    }

    closedir(directory);
    return result;
}

esp_err_t object_store_fs_get_delete_scope(
    const char *object_id,
    object_store_delete_scope_t *scope)
{
    if (scope == NULL || !object_format_id_is_valid(object_id)) return ESP_ERR_INVALID_ARG;
    memset(scope, 0, sizeof(*scope));

    char object_path[OBJECT_STORE_FS_PATH_SIZE];
    esp_err_t err = object_store_fs_object_path(object_id, object_path, sizeof(object_path));
    if (err != ESP_OK) return err;

    struct stat info;
    if (stat(object_path, &info) != 0 || !object_store_fs_is_directory_mode(&info)) {
        return ESP_ERR_NOT_FOUND;
    }
    return object_store_fs_count_delete_scope_directory(object_path, true, false, scope);
}
