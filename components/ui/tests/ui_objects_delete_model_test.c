#include <assert.h>
#include <string.h>

#include "ui_objects_delete_model.h"

static void test_zero_counts(void)
{
    const object_store_delete_scope_t scope = {0U, 0U, 0U};
    ui_objects_delete_model_t model;
    ui_objects_delete_model_build(true, &scope, &model);
    assert(model.available);
    assert(model.line_count == 3U);
    assert(model.lines[0].key == UI_I18N_KEY_OBJECTS_DELETE_SCOPE_SESSIONS);
    assert(model.lines[0].count == 0U);
    assert(model.lines[1].key == UI_I18N_KEY_OBJECTS_DELETE_SCOPE_ARTIFACTS);
    assert(model.lines[1].count == 0U);
    assert(model.lines[2].key == UI_I18N_KEY_OBJECTS_DELETE_SCOPE_RUNS);
    assert(model.lines[2].count == 0U);
}

static void test_one_and_many_counts(void)
{
    ui_objects_delete_model_t model;
    const object_store_delete_scope_t one = {1U, 1U, 1U};
    ui_objects_delete_model_build(true, &one, &model);
    assert(model.line_count == 3U);
    assert(model.lines[0].count == 1U && model.lines[1].count == 1U && model.lines[2].count == 1U);

    const object_store_delete_scope_t many = {23U, 47U, 18U};
    ui_objects_delete_model_build(true, &many, &model);
    assert(model.line_count == 3U);
    assert(model.lines[0].count == 23U && model.lines[1].count == 47U && model.lines[2].count == 18U);
}

static void test_unavailable_scope_is_explicit(void)
{
    ui_objects_delete_model_t model;
    ui_objects_delete_model_build(false, NULL, &model);
    assert(!model.available);
    assert(model.line_count == 1U);
    assert(model.lines[0].key == UI_I18N_KEY_OBJECTS_DELETE_SCOPE_UNAVAILABLE);
}

int main(void)
{
    test_zero_counts();
    test_one_and_many_counts();
    test_unavailable_scope_is_explicit();
    return 0;
}
