#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

#include "object_store_internal.h"
#include "storage_manager.h"

static unsigned remove_tree_calls;
static char removed_path[OBJECT_STORE_FS_PATH_SIZE];

bool storage_manager_path_exists(const char *path)
{
    struct stat info;
    return path != NULL && stat(path, &info) == 0;
}

esp_err_t storage_manager_create_directory(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return mkdir(path, 0775) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++remove_tree_calls;
    snprintf(removed_path, sizeof(removed_path), "%s", path != NULL ? path : "");
    return ESP_OK;
}

#include "../object_store_fs.c"
#include "../object_store_fs_ids.c"
#include "../object_store_fs_objects.c"
#include "../object_store_fs_list.c"
#include "../object_store_delete_fs.c"
#include "../../object_format/object_format.c"

static void remove_known_tree(void)
{
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/object.json");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007/object.json");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}

static void make_base_tree(void)
{
    remove_known_tree();
    assert(mkdir(STORAGE_MANAGER_MOUNT_POINT, 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_ROOT_PATH, 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH, 0775) == 0);
}

static void test_path_validation_and_copy(void)
{
    char path[OBJECT_STORE_FS_PATH_SIZE];
    assert(object_store_fs_object_path("OBJ_0001", path, sizeof(path)) == ESP_OK);
    assert(strcmp(path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(object_store_fs_metadata_path("OBJ_0001", path, sizeof(path)) == ESP_OK);
    assert(strcmp(path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/object.json") == 0);
    assert(object_store_fs_object_path("OBJ_0001/..", path, sizeof(path)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_metadata_path("OBJ_001", path, sizeof(path)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_object_path(NULL, path, sizeof(path)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_metadata_path("OBJ_0001", NULL, sizeof(path)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_object_path("OBJ_0001", path, 1U) == ESP_ERR_INVALID_SIZE);

    char copied[OBJECT_STORE_ID_SIZE] = {0};
    object_store_fs_copy_id(copied, sizeof(copied), "OBJ_0042");
    assert(strcmp(copied, "OBJ_0042") == 0);
    memset(copied, 'x', sizeof(copied));
    object_store_fs_copy_id(copied, OBJECT_FORMAT_ID_TEXT_LENGTH, "OBJ_0042");
    assert(copied[0] == 'x');
}

static void test_next_id_ignores_invalid_entries_and_files(void)
{
    make_base_tree();
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002", 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007", 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad", 0775) == 0);
    FILE *file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999", "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);

    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    assert(object_store_fs_next_id(object_id, sizeof(object_id)) == ESP_OK);
    assert(strcmp(object_id, "OBJ_0008") == 0);
    assert(object_store_fs_next_id(NULL, sizeof(object_id)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_next_id(object_id, OBJECT_STORE_ID_SIZE - 1U) == ESP_ERR_INVALID_ARG);
}

static void test_create_exists_list_and_delete_contracts(void)
{
    make_base_tree();
    assert(object_store_fs_create_layout("OBJ_0002") == ESP_OK);
    assert(storage_manager_path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/sessions"));
    assert(object_store_fs_create_layout("OBJ_0002/..") == ESP_ERR_INVALID_ARG);

    FILE *metadata = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/object.json", "wb");
    assert(metadata != NULL);
    assert(fputs("{}", metadata) >= 0);
    assert(fclose(metadata) == 0);
    assert(object_store_fs_object_exists("OBJ_0002"));
    assert(!object_store_fs_object_exists("OBJ_0003"));
    assert(!object_store_fs_object_exists("OBJ_0002/.."));

    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007", 0775) == 0);
    object_store_summary_t objects[4] = {0};
    size_t count = 0U;
    assert(object_store_fs_list(objects, 4U, &count, "OBJ_0007") == ESP_OK);
    assert(count == 2U);
    assert(strcmp(objects[0].id, "OBJ_0002") == 0);
    assert(!objects[0].active);
    assert(strcmp(objects[1].id, "OBJ_0007") == 0);
    assert(objects[1].active);
    assert(object_store_fs_list(NULL, 4U, &count, NULL) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_list(objects, 0U, &count, NULL) == ESP_ERR_INVALID_ARG);

    remove_tree_calls = 0U;
    assert(object_store_fs_delete_object("OBJ_0002") == ESP_OK);
    assert(remove_tree_calls == 1U);
    assert(strcmp(removed_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002") == 0);
    assert(object_store_fs_delete_object("OBJ_0002/..") == ESP_ERR_INVALID_ARG);
    remove_known_tree();
}

int main(void)
{
    test_path_validation_and_copy();
    test_next_id_ignores_invalid_entries_and_files();
    test_create_exists_list_and_delete_contracts();
    return 0;
}
