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

#include "../object_store_fs.c"
#include "../object_store_archive_policy.c"
#include "../object_store_fs_list.c"
#include "../../object_format/object_format.c"


static void object_path(int number, char *path, size_t path_size)
{
    snprintf(
        path,
        path_size,
        "%s/OBJ_%04d",
        STORAGE_MANAGER_OBJECTS_PATH,
        number
    );
}


static void remove_fixture(void)
{
    char path[OBJECT_STORE_FS_PATH_SIZE];

    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad");

    for (int number = 1; number <= 40; ++number) {
        object_path(number, path, sizeof(path));
        (void)rmdir(path);
    }

    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}


static void make_base_tree(void)
{
    remove_fixture();
    assert(mkdir(STORAGE_MANAGER_MOUNT_POINT, 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_ROOT_PATH, 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH, 0775) == 0);
}


static void make_objects(bool descending)
{
    char path[OBJECT_STORE_FS_PATH_SIZE];

    if (descending) {
        for (int number = 40; number >= 1; --number) {
            object_path(number, path, sizeof(path));
            assert(mkdir(path, 0775) == 0);
        }
    }
    else {
        for (int number = 1; number <= 40; ++number) {
            object_path(number, path, sizeof(path));
            assert(mkdir(path, 0775) == 0);
        }
    }

    FILE *regular = fopen(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999",
        "wb"
    );
    assert(regular != NULL);
    assert(fclose(regular) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad", 0775) == 0);
}


static void assert_ascending(
    const object_store_summary_t *objects,
    size_t count
)
{
    for (size_t i = 1U; i < count; ++i) {
        assert(strcmp(objects[i - 1U].id, objects[i].id) < 0);
    }
}


static void test_top_32_is_enumeration_order_independent(void)
{
    object_store_summary_t ascending[OBJECT_STORE_MAX_OBJECTS] = {0};
    object_store_summary_t descending[OBJECT_STORE_MAX_OBJECTS] = {0};
    size_t ascending_count = 0U;
    size_t descending_count = 0U;

    make_base_tree();
    make_objects(false);
    assert(object_store_fs_list(
        ascending,
        OBJECT_STORE_MAX_OBJECTS,
        &ascending_count,
        NULL
    ) == ESP_OK);

    make_base_tree();
    make_objects(true);
    assert(object_store_fs_list(
        descending,
        OBJECT_STORE_MAX_OBJECTS,
        &descending_count,
        NULL
    ) == ESP_OK);

    assert(ascending_count == OBJECT_STORE_MAX_OBJECTS);
    assert(descending_count == ascending_count);
    assert(strcmp(ascending[0].id, "OBJ_0009") == 0);
    assert(strcmp(ascending[31].id, "OBJ_0040") == 0);
    assert_ascending(ascending, ascending_count);

    for (size_t i = 0U; i < ascending_count; ++i) {
        assert(strcmp(ascending[i].id, descending[i].id) == 0);
        assert(!ascending[i].active);
        assert(!descending[i].active);
    }
}


static void test_old_active_is_pinned_into_full_snapshot(void)
{
    object_store_summary_t objects[OBJECT_STORE_MAX_OBJECTS] = {0};
    size_t count = 0U;

    make_base_tree();
    make_objects(false);
    assert(object_store_fs_list(
        objects,
        OBJECT_STORE_MAX_OBJECTS,
        &count,
        "OBJ_0001"
    ) == ESP_OK);

    assert(count == OBJECT_STORE_MAX_OBJECTS);
    assert(strcmp(objects[0].id, "OBJ_0001") == 0);
    assert(objects[0].active);
    assert(strcmp(objects[1].id, "OBJ_0010") == 0);
    assert(strcmp(objects[31].id, "OBJ_0040") == 0);
    assert_ascending(objects, count);

    for (size_t i = 1U; i < count; ++i) {
        assert(!objects[i].active);
    }
}


static void test_requested_capacity_and_active_pin(void)
{
    object_store_summary_t objects[OBJECT_STORE_MAX_OBJECTS] = {0};
    size_t count = 0U;

    make_base_tree();
    make_objects(false);
    assert(object_store_fs_list(
        objects,
        4U,
        &count,
        "OBJ_0001"
    ) == ESP_OK);

    assert(count == 4U);
    assert(strcmp(objects[0].id, "OBJ_0001") == 0);
    assert(objects[0].active);
    assert(strcmp(objects[1].id, "OBJ_0038") == 0);
    assert(strcmp(objects[2].id, "OBJ_0039") == 0);
    assert(strcmp(objects[3].id, "OBJ_0040") == 0);
}


static void test_regular_file_and_unknown_type_fallback(void)
{
    make_base_tree();

    char path[OBJECT_STORE_FS_PATH_SIZE];
    object_path(1, path, sizeof(path));
    assert(mkdir(path, 0775) == 0);

    FILE *regular = fopen(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999",
        "wb"
    );
    assert(regular != NULL);
    assert(fclose(regular) == 0);

#if defined(DT_UNKNOWN)
    struct dirent entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.d_name, sizeof(entry.d_name), "%s", "OBJ_0001");
    entry.d_type = DT_UNKNOWN;
    assert(object_store_fs_list_entry_is_directory(&entry));

    snprintf(entry.d_name, sizeof(entry.d_name), "%s", "OBJ_9999");
    entry.d_type = DT_UNKNOWN;
    assert(!object_store_fs_list_entry_is_directory(&entry));
#endif

    object_store_summary_t objects[4] = {0};
    size_t count = 0U;
    assert(object_store_fs_list(objects, 4U, &count, NULL) == ESP_OK);
    assert(count == 1U);
    assert(strcmp(objects[0].id, "OBJ_0001") == 0);
}


int main(void)
{
    test_top_32_is_enumeration_order_independent();
    test_old_active_is_pinned_into_full_snapshot();
    test_requested_capacity_and_active_pin();
    test_regular_file_and_unknown_type_fallback();
    remove_fixture();
    puts("OBJECT_STORE_ARCHIVE_HOST_PASS");
    return 0;
}
