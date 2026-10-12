#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object_store.h"
#include "ui_i18n.h"

#define UI_OBJECTS_DELETE_MODEL_MAX_LINES 3U

typedef struct
{
    ui_i18n_key_t key;
    uint32_t count;
} ui_objects_delete_model_line_t;

typedef struct
{
    bool available;
    size_t line_count;
    ui_objects_delete_model_line_t lines[UI_OBJECTS_DELETE_MODEL_MAX_LINES];
} ui_objects_delete_model_t;

void ui_objects_delete_model_build(
    bool available,
    const object_store_delete_scope_t *scope,
    ui_objects_delete_model_t *model);
