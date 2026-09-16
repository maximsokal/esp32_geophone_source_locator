#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "object_store_internal.h"

#define TEST_ERR_NEXT      0x201
#define TEST_ERR_LAYOUT    0x202
#define TEST_ERR_METADATA  0x203
#define TEST_ERR_ACTIVE    0x204
#define TEST_ERR_CLEANUP   0x205

typedef enum
{
    FAIL_NONE = 0,
    FAIL_NEXT,
    FAIL_LAYOUT_COLLISION,
    FAIL_LAYOUT_AFTER_ROOT,
    FAIL_METADATA,
    FAIL_ACTIVE,
} fail_stage_t;

static fail_stage_t fail_stage;
static bool fail_cleanup;
static char call_order[64];
static size_t call_count;
static bool runtime_published;
static bool cache_created;
static bool cache_active;
static bool cache_reset;
static bool cleanup_called;
static char metadata_name[OBJECT_STORE_NAME_SIZE];
static char cache_name[OBJECT_STORE_NAME_SIZE];

static void note(char call)
{
    assert(call_count + 1U < sizeof(call_order));
    call_order[call_count++] = call;
    call_order[call_count] = '\0';
}

static void reset_state(fail_stage_t stage, bool cleanup_should_fail)
{
    fail_stage = stage;
    fail_cleanup = cleanup_should_fail;
    memset(call_order, 0, sizeof(call_order));
    call_count = 0U;
    runtime_published = false;
    cache_created = false;
    cache_active = false;
    cache_reset = false;
    cleanup_called = false;
    metadata_name[0] = '\0';
    cache_name[0] = '\0';
}

esp_err_t object_store_internal_validate_name(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    const size_t length = strlen(name);
    if (length == 0U || length >= OBJECT_STORE_NAME_SIZE) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 0x20U) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t object_store_internal_begin_file_operation(void)
{
    note('B');
    return ESP_OK;
}

void object_store_internal_end_file_operation(void)
{
    note('E');
}

esp_err_t object_store_fs_next_id(char *object_id, size_t object_id_size)
{
    note('N');
    if (fail_stage == FAIL_NEXT) return TEST_ERR_NEXT;
    assert(object_id_size >= OBJECT_STORE_ID_SIZE);
    snprintf(object_id, object_id_size, "OBJ_0042");
    return ESP_OK;
}

esp_err_t object_store_fs_create_layout(const char *object_id, bool *root_created)
{
    note('L');
    assert(strcmp(object_id, "OBJ_0042") == 0);
    assert(root_created != NULL);
    *root_created = false;
    if (fail_stage == FAIL_LAYOUT_COLLISION) return TEST_ERR_LAYOUT;
    *root_created = true;
    if (fail_stage == FAIL_LAYOUT_AFTER_ROOT) return TEST_ERR_LAYOUT;
    return ESP_OK;
}

esp_err_t object_store_json_write_metadata(const char *object_id, const char *initial_name)
{
    note('M');
    assert(strcmp(object_id, "OBJ_0042") == 0);
    snprintf(
        metadata_name,
        sizeof(metadata_name),
        "%s",
        initial_name != NULL ? initial_name : object_id
    );
    return fail_stage == FAIL_METADATA ? TEST_ERR_METADATA : ESP_OK;
}

esp_err_t object_store_fs_save_active(const char *object_id)
{
    note('A');
    assert(strcmp(object_id, "OBJ_0042") == 0);
    return fail_stage == FAIL_ACTIVE ? TEST_ERR_ACTIVE : ESP_OK;
}

esp_err_t object_store_fs_delete_object(const char *object_id)
{
    note('D');
    cleanup_called = true;
    assert(strcmp(object_id, "OBJ_0042") == 0);
    return fail_cleanup ? TEST_ERR_CLEANUP : ESP_OK;
}

void object_store_profile_cache_reset(void)
{
    note('R');
    cache_reset = true;
}

void object_store_internal_set_active_id(const char *object_id)
{
    note('S');
    runtime_published = true;
    assert(strcmp(object_id, "OBJ_0042") == 0);
}

void object_store_profile_cache_note_create_locked(const char *object_id, const char *name)
{
    note('C');
    cache_created = true;
    assert(strcmp(object_id, "OBJ_0042") == 0);
    snprintf(cache_name, sizeof(cache_name), "%s", name);
}

void object_store_profile_cache_note_active_locked(const char *object_id)
{
    note('P');
    cache_active = true;
    assert(strcmp(object_id, "OBJ_0042") == 0);
}

void object_store_internal_fill_summary(
    object_store_summary_t *object,
    const char *object_id,
    bool active
)
{
    snprintf(object->id, sizeof(object->id), "%s", object_id);
    snprintf(object->name, sizeof(object->name), "%s", object_id);
    object->active = active;
}

esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    (void)objects;
    (void)max_objects;
    (void)object_count;
    (void)default_cable_lengths_m;
    return ESP_FAIL;
}

#include "../object_store_operations.c"

static void assert_not_published(void)
{
    assert(!runtime_published);
    assert(!cache_created);
    assert(!cache_active);
}

static void test_validation_precedes_storage(void)
{
    reset_state(FAIL_NONE, false);
    assert(object_store_create_named("", NULL) == ESP_ERR_INVALID_SIZE);
    assert(call_count == 0U);

    reset_state(FAIL_NONE, false);
    assert(object_store_create_named("bad\nname", NULL) == ESP_ERR_INVALID_ARG);
    assert(call_count == 0U);
}

static void test_root_collision_is_never_rolled_back(void)
{
    reset_state(FAIL_LAYOUT_COLLISION, false);
    assert(object_store_create_named("Named", NULL) == TEST_ERR_LAYOUT);
    assert(strcmp(call_order, "BNLE") == 0);
    assert(!cleanup_called);
    assert_not_published();
}

static void test_sessions_failure_rolls_back_owned_root(void)
{
    reset_state(FAIL_LAYOUT_AFTER_ROOT, false);
    assert(object_store_create_named("Named", NULL) == TEST_ERR_LAYOUT);
    assert(strcmp(call_order, "BNLDE") == 0);
    assert(cleanup_called);
    assert_not_published();
}

static void test_metadata_failure_rolls_back_before_active(void)
{
    reset_state(FAIL_METADATA, false);
    assert(object_store_create_named("Named", NULL) == TEST_ERR_METADATA);
    assert(strcmp(call_order, "BNLMDE") == 0);
    assert(cleanup_called);
    assert_not_published();
}

static void test_active_failure_preserves_publication_boundary(void)
{
    reset_state(FAIL_ACTIVE, false);
    assert(object_store_create_named("Named", NULL) == TEST_ERR_ACTIVE);
    assert(strcmp(call_order, "BNLMADE") == 0);
    assert(cleanup_called);
    assert_not_published();
}

static void test_cleanup_failure_keeps_primary_error_and_resets_cache(void)
{
    reset_state(FAIL_ACTIVE, true);
    assert(object_store_create_named("Named", NULL) == TEST_ERR_ACTIVE);
    assert(strcmp(call_order, "BNLMADRE") == 0);
    assert(cleanup_called);
    assert(cache_reset);
    assert_not_published();
}

static void test_success_named_publishes_after_active(void)
{
    reset_state(FAIL_NONE, false);
    object_store_summary_t created = {0};
    assert(object_store_create_named("Final name", &created) == ESP_OK);
    assert(strcmp(call_order, "BNLMASCPE") == 0);
    assert(strcmp(metadata_name, "Final name") == 0);
    assert(strcmp(cache_name, "Final name") == 0);
    assert(strcmp(created.id, "OBJ_0042") == 0);
    assert(strcmp(created.name, "Final name") == 0);
    assert(created.active);
    assert(runtime_published && cache_created && cache_active);
    assert(!cleanup_called);
}

static void test_success_unnamed_uses_canonical_name(void)
{
    reset_state(FAIL_NONE, false);
    object_store_summary_t created = {0};
    assert(object_store_create(&created) == ESP_OK);
    assert(strcmp(metadata_name, "OBJ_0042") == 0);
    assert(strcmp(cache_name, "OBJ_0042") == 0);
    assert(strcmp(created.name, "OBJ_0042") == 0);
}

int main(void)
{
    test_validation_precedes_storage();
    test_root_collision_is_never_rolled_back();
    test_sessions_failure_rolls_back_owned_root();
    test_metadata_failure_rolls_back_before_active();
    test_active_failure_preserves_publication_boundary();
    test_cleanup_failure_keeps_primary_error_and_resets_cache();
    test_success_named_publishes_after_active();
    test_success_unnamed_uses_canonical_name();
    return 0;
}
