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

static char active_id[OBJECT_STORE_ID_SIZE] = "OBJ_0001";
static esp_err_t metadata_validation_result = ESP_OK;
static esp_err_t active_save_result = ESP_OK;
static char activation_trace[32];
static size_t activation_trace_length;

static void activation_trace_push(char event)
{
    assert(activation_trace_length + 1U < sizeof(activation_trace));
    activation_trace[activation_trace_length++] = event;
    activation_trace[activation_trace_length] = '\0';
}

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

esp_err_t storage_manager_create_directory_exclusive(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++remove_tree_calls;
    snprintf(removed_path, sizeof(removed_path), "%s", path != NULL ? path : "");
    return ESP_OK;
}

esp_err_t object_store_internal_begin_file_operation(void)
{
    activation_trace_push('B');
    return ESP_OK;
}

void object_store_internal_end_file_operation(void)
{
    activation_trace_push('E');
}

esp_err_t object_store_internal_begin_state_operation(void)
{
    return ESP_OK;
}

void object_store_internal_end_state_operation(void)
{
}

esp_err_t object_store_json_read_name_locked(
    const char *object_id,
    char *name,
    size_t name_size
)
{
    activation_trace_push('V');
    if (metadata_validation_result != ESP_OK) {
        return metadata_validation_result;
    }
    (void)snprintf(name, name_size, "%s", object_id);
    return ESP_OK;
}

esp_err_t object_store_fs_save_active(const char *object_id)
{
    (void)object_id;
    activation_trace_push('A');
    return active_save_result;
}

void object_store_internal_set_active_id(const char *object_id)
{
    activation_trace_push('R');
    (void)snprintf(active_id, sizeof(active_id), "%s", object_id);
}

const char *object_store_internal_active_id(void)
{
    return active_id;
}

void object_store_internal_clear_active_id(void)
{
    active_id[0] = '\0';
}

void object_store_profile_cache_invalidate_membership_locked(void)
{
    activation_trace_push('C');
}

void object_store_profile_cache_note_active_locked(const char *object_id)
{
    (void)object_id;
}

void object_store_profile_cache_note_delete_locked(const char *object_id)
{
    (void)object_id;
}

esp_err_t object_store_fs_clear_active(void)
{
    return ESP_OK;
}

#include "../object_store_fs.c"
#include "../object_store_fs_ids.c"
#include "../object_store_fs_objects.c"
#include "../object_store_archive_policy.c"
#include "../object_store_fs_list.c"
#include "../object_store_delete_fs.c"
#include "../object_store_lease.c"
#include "../object_store_mutations.c"
#include "../../object_format/object_format.c"

static void remove_known_tree(void)
{
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/object.json");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/object.json");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0003");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007/object.json");
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad");
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

static void write_metadata(const char *object_id)
{
    char path[OBJECT_STORE_FS_PATH_SIZE];
    assert(object_store_fs_metadata_path(object_id, path, sizeof(path)) == ESP_OK);
    FILE *metadata = fopen(path, "wb");
    assert(metadata != NULL);
    assert(fputs("{}", metadata) >= 0);
    assert(fclose(metadata) == 0);
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

static void test_next_id_uses_only_committed_directories(void)
{
    make_base_tree();
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002", 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0007", 0775) == 0);
    assert(mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_bad", 0775) == 0);
    write_metadata("OBJ_0002");

    FILE *file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_9999", "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);

    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    assert(object_store_fs_next_id(object_id, sizeof(object_id)) == ESP_OK);
    assert(strcmp(object_id, "OBJ_0003") == 0);
    assert(object_store_fs_next_id(NULL, sizeof(object_id)) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_next_id(object_id, OBJECT_STORE_ID_SIZE - 1U) == ESP_ERR_INVALID_ARG);
}

static void test_exclusive_layout_reports_ownership(void)
{
    make_base_tree();

    bool root_created = true;
    assert(object_store_fs_create_layout("OBJ_0001", &root_created) == ESP_OK);
    assert(root_created);
    assert(storage_manager_path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions"));

    root_created = true;
    assert(object_store_fs_create_layout("OBJ_0001", &root_created) == ESP_ERR_INVALID_STATE);
    assert(!root_created);

    FILE *file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0003", "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);
    root_created = true;
    assert(object_store_fs_create_layout("OBJ_0003", &root_created) == ESP_ERR_INVALID_STATE);
    assert(!root_created);
    assert(storage_manager_path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0003"));

    assert(object_store_fs_create_layout("OBJ_0002", NULL) == ESP_ERR_INVALID_ARG);
    assert(object_store_fs_create_layout("OBJ_0002/..", &root_created) == ESP_ERR_INVALID_ARG);
}

static void test_create_exists_list_and_delete_contracts(void)
{
    make_base_tree();
    bool root_created = false;
    assert(object_store_fs_create_layout("OBJ_0002", &root_created) == ESP_OK);
    assert(root_created);
    assert(storage_manager_path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002/sessions"));
    write_metadata("OBJ_0002");
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

static void test_set_active_validates_before_commit(void)
{
    make_base_tree();
    bool root_created = false;
    assert(object_store_fs_create_layout("OBJ_0002", &root_created) == ESP_OK);
    assert(root_created);
    write_metadata("OBJ_0002");

    (void)snprintf(active_id, sizeof(active_id), "%s", "OBJ_0001");
    metadata_validation_result = ESP_ERR_INVALID_RESPONSE;
    active_save_result = ESP_OK;
    activation_trace_length = 0U;
    activation_trace[0] = '\0';

    assert(object_store_set_active("OBJ_0002") == ESP_ERR_INVALID_RESPONSE);
    assert(strcmp(active_id, "OBJ_0001") == 0);
    assert(strcmp(activation_trace, "BVE") == 0);

    metadata_validation_result = ESP_OK;
    active_save_result = ESP_FAIL;
    activation_trace_length = 0U;
    activation_trace[0] = '\0';

    assert(object_store_set_active("OBJ_0002") == ESP_FAIL);
    assert(strcmp(active_id, "OBJ_0001") == 0);
    assert(strcmp(activation_trace, "BVAE") == 0);

    active_save_result = ESP_OK;
    activation_trace_length = 0U;
    activation_trace[0] = '\0';

    assert(object_store_set_active("OBJ_0002") == ESP_OK);
    assert(strcmp(active_id, "OBJ_0002") == 0);
    assert(strcmp(activation_trace, "BVARCE") == 0);

    remove_known_tree();
}

static void test_object_lifetime_leases_block_delete(void)
{
    make_base_tree();
    bool root_created = false;
    assert(object_store_fs_create_layout("OBJ_0001", &root_created) == ESP_OK);
    assert(root_created);
    write_metadata("OBJ_0001");
    root_created = false;
    assert(object_store_fs_create_layout("OBJ_0002", &root_created) == ESP_OK);
    assert(root_created);
    write_metadata("OBJ_0002");

    object_store_lease_reset_locked();
    (void)snprintf(active_id, sizeof(active_id), "%s", "OBJ_0001");
    activation_trace_length = 0U;
    activation_trace[0] = '\0';
    remove_tree_calls = 0U;

    object_store_lease_t first = {0};
    object_store_lease_t second = {0};
    assert(object_store_lease_acquire("OBJ_0001", &first) == ESP_OK);
    assert(object_store_lease_acquire("OBJ_0001", &second) == ESP_OK);
    assert(first.token_id != 0U);
    assert(second.token_id != 0U);
    assert(first.token_id != second.token_id);

    bool leased = false;
    assert(object_store_object_is_leased("OBJ_0001", &leased) == ESP_OK);
    assert(leased);

    assert(object_store_delete("OBJ_0001") == ESP_ERR_INVALID_STATE);
    assert(strcmp(active_id, "OBJ_0001") == 0);
    assert(remove_tree_calls == 0U);

    assert(object_store_delete("OBJ_0002") == ESP_OK);
    assert(remove_tree_calls == 1U);
    assert(strcmp(removed_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0002") == 0);

    object_store_lease_t stale = first;
    assert(object_store_lease_release(&first) == ESP_OK);
    assert(first.token_id == 0U);
    assert(object_store_lease_release(&stale) == ESP_ERR_INVALID_STATE);
    assert(object_store_object_is_leased("OBJ_0001", &leased) == ESP_OK);
    assert(leased);

    assert(object_store_lease_release(&second) == ESP_OK);
    assert(second.token_id == 0U);
    assert(object_store_lease_release(&second) == ESP_OK);
    assert(object_store_object_is_leased("OBJ_0001", &leased) == ESP_OK);
    assert(!leased);

    assert(object_store_delete("OBJ_0001") == ESP_OK);
    assert(remove_tree_calls == 2U);
    assert(active_id[0] == '\0');

    remove_known_tree();
}

int main(void)
{
    test_path_validation_and_copy();
    test_next_id_uses_only_committed_directories();
    test_exclusive_layout_reports_ownership();
    test_create_exists_list_and_delete_contracts();
    test_set_active_validates_before_commit();
    test_object_lifetime_leases_block_delete();
    return 0;
}
