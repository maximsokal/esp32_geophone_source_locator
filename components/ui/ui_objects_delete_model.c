#include "ui_objects_delete_model.h"

#include <string.h>

void ui_objects_delete_model_build(
    bool available,
    const object_store_delete_scope_t *scope,
    ui_objects_delete_model_t *model)
{
    if (model == NULL) return;
    memset(model, 0, sizeof(*model));
    if (!available || scope == NULL) {
        model->line_count = 1U;
        model->lines[0].key = UI_I18N_KEY_OBJECTS_DELETE_SCOPE_UNAVAILABLE;
        return;
    }

    model->available = true;
    model->line_count = UI_OBJECTS_DELETE_MODEL_MAX_LINES;
    model->lines[0] = (ui_objects_delete_model_line_t){
        .key = UI_I18N_KEY_OBJECTS_DELETE_SCOPE_SESSIONS,
        .count = scope->sessions,
    };
    model->lines[1] = (ui_objects_delete_model_line_t){
        .key = UI_I18N_KEY_OBJECTS_DELETE_SCOPE_ARTIFACTS,
        .count = scope->artifacts,
    };
    model->lines[2] = (ui_objects_delete_model_line_t){
        .key = UI_I18N_KEY_OBJECTS_DELETE_SCOPE_RUNS,
        .count = scope->localization_runs,
    };
}
