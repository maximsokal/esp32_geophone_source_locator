#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"


/*
 * Object names are persisted as UTF-8 bytes and may use the full
 * OBJECT_FORMAT_NAME_SIZE contract. Keep enough room for the complete object
 * id/name plus the longest decoration currently rendered by this screen.
 * This avoids format-truncation warnings without silently shortening names.
 */
#define UI_OBJECTS_TEXT_SIZE \
    (OBJECT_FORMAT_ID_SIZE + OBJECT_FORMAT_NAME_SIZE + sizeof("  [АКТИВНИЙ]"))


static void ui_objects_render_notice(lv_obj_t *screen, int y)
{
    ui_context_t *context = ui_context();

    if (context->notice[0] == '\0') {
        return;
    }

    (void)ui_create_label_locked(
        screen,
        12,
        y,
        296,
        context->notice,
        UI_COLOR_WARNING
    );
}


static void ui_objects_render_menu(lv_obj_t *screen)
{
    ui_context_t *context = ui_context();
    object_service_summary_t active;

    lv_obj_t *active_card = ui_create_card_locked(
        screen, 10, 36, 300, 26
    );

    if (active_card == NULL) {
        return;
    }

    if (object_service_get_active(&active) == ESP_OK) {
        char text[UI_OBJECTS_TEXT_SIZE];
        snprintf(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_OBJECTS_ACTIVE),
            active.id,
            active.name
        );

        (void)ui_create_label_locked(
            active_card,
            10,
            6,
            280,
            text,
            UI_COLOR_ACCENT
        );
    } else {
        (void)ui_create_label_locked(
            active_card,
            10,
            6,
            280,
            ui_tr(UI_I18N_KEY_OBJECTS_ACTIVE_NONE),
            UI_COLOR_MUTED
        );
    }

    static const ui_i18n_key_t actions[4] = {
        UI_I18N_KEY_OBJECTS_NEW,
        UI_I18N_KEY_OBJECTS_SET_ACTIVE,
        UI_I18N_KEY_OBJECTS_DELETE,
        UI_I18N_KEY_TFT_WORKFLOW_ACTION_START_MEASUREMENT,
    };

    const bool have_objects =
        context->objects_loaded &&
        context->object_count > 0;

    for (size_t i = 0; i < 4; ++i) {
        const bool enabled =
            i == 0 ||
            have_objects;

        (void)ui_create_row_locked(
            screen,
            64 + (int)(i * 33U),
            ui_tr(actions[i]),
            i == context->object_action,
            enabled
        );
    }

    ui_objects_render_notice(screen, 197);
}


static void ui_objects_render_list(lv_obj_t *screen)
{
    ui_context_t *context = ui_context();

    const char *caption = context->objects_mode == UI_OBJECTS_SELECT_ACTIVE
        ? ui_tr(UI_I18N_KEY_OBJECTS_SELECT_ACTIVATE)
        : context->objects_mode == UI_OBJECTS_SELECT_WORKFLOW
            ? ui_tr(UI_I18N_KEY_TFT_WORKFLOW_OBJECT_SELECT)
            : ui_tr(UI_I18N_KEY_OBJECTS_SELECT_DELETE);

    (void)ui_create_label_locked(
        screen,
        12,
        42,
        296,
        caption,
        UI_COLOR_MUTED
    );

    if (context->object_count == 0) {
        (void)ui_create_label_locked(
            screen,
            12,
            84,
            296,
            ui_tr(UI_I18N_KEY_OBJECTS_NONE),
            UI_COLOR_WARNING
        );
        ui_objects_render_notice(screen, 196);
        return;
    }

    const size_t page_start =
        (context->object_index / 4U) * 4U;

    for (size_t row = 0; row < 4; ++row) {
        const size_t index = page_start + row;

        if (index >= context->object_count) {
            break;
        }

        char text[UI_OBJECTS_TEXT_SIZE];
        snprintf(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_OBJECTS_LIST_ENTRY),
            context->objects[index].id,
            context->objects[index].name,
            context->objects[index].active
                ? ui_tr(UI_I18N_KEY_OBJECTS_ACTIVE_MARKER)
                : ""
        );

        (void)ui_create_row_locked(
            screen,
            62 + (int)(row * 34U),
            text,
            index == context->object_index,
            true
        );
    }

    ui_objects_render_notice(screen, 196);
}


static void ui_objects_render_delete_confirm(lv_obj_t *screen)
{
    ui_context_t *context = ui_context();

    lv_obj_t *card = ui_create_card_locked(
        screen, 16, 62, 288, 112
    );

    if (card == NULL) {
        return;
    }

    (void)ui_create_label_locked(
        card,
        14,
        14,
        260,
        ui_tr(UI_I18N_KEY_OBJECTS_CONFIRM_DELETE),
        UI_COLOR_ERROR
    );

    if (
        context->object_count > 0 &&
        context->object_index < context->object_count
    ) {
        char text[UI_OBJECTS_TEXT_SIZE];
        snprintf(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_OBJECTS_NAME_PAIR),
            context->objects[context->object_index].id,
            context->objects[context->object_index].name
        );

        (void)ui_create_label_locked(
            card,
            14,
            45,
            260,
            text,
            UI_COLOR_TEXT
        );
    }

    (void)ui_create_label_locked(
        card,
        14,
        75,
        260,
        ui_tr(UI_I18N_KEY_OBJECTS_CONFIRM_FOOTER),
        UI_COLOR_WARNING
    );

    ui_objects_render_notice(screen, 196);
}


static void ui_objects_cache_created(
    ui_context_t *context,
    const object_service_summary_t *object
)
{
    if (
        !context->objects_loaded ||
        object == NULL
    ) {
        return;
    }

    for (size_t i = 0; i < context->object_count; ++i) {
        if (strcmp(context->objects[i].id, object->id) == 0) {
            context->objects[i] = *object;
            return;
        }
    }

    if (context->object_count >= OBJECT_SERVICE_MAX_OBJECTS) {
        context->objects_loaded = false;
        return;
    }

    context->objects[context->object_count] = *object;
    ++context->object_count;
}


static void ui_objects_cache_active(
    ui_context_t *context,
    const char *object_id
)
{
    if (
        !context->objects_loaded ||
        object_id == NULL
    ) {
        return;
    }

    bool found = false;
    for (size_t i = 0; i < context->object_count; ++i) {
        if (strcmp(context->objects[i].id, object_id) == 0) {
            found = true;
            break;
        }
    }

    /* ACTIVE is required to be part of every authoritative bounded snapshot.
     * If an external mutation names an object missing from this copied list,
     * the copy is stale; do not leave a loaded list with every row inactive. */
    if (!found) {
        context->objects_loaded = false;
        return;
    }

    for (size_t i = 0; i < context->object_count; ++i) {
        context->objects[i].active =
            strcmp(context->objects[i].id, object_id) == 0;
    }
}


static void ui_objects_cache_deleted(
    ui_context_t *context,
    const char *object_id
)
{
    if (
        !context->objects_loaded ||
        object_id == NULL
    ) {
        return;
    }

    const bool snapshot_was_full =
        context->object_count >= OBJECT_SERVICE_MAX_OBJECTS;

    for (size_t i = 0; i < context->object_count; ++i) {
        if (strcmp(context->objects[i].id, object_id) != 0) {
            continue;
        }

        /* When a full bounded snapshot loses one visible member, an older
         * archived object may need to backfill it. Only a fresh LIST knows the
         * canonical replacement, so mark this copied list stale. */
        if (snapshot_was_full) {
            context->objects_loaded = false;
            return;
        }

        if (i + 1U < context->object_count) {
            memmove(
                &context->objects[i],
                &context->objects[i + 1U],
                (context->object_count - i - 1U) *
                    sizeof(context->objects[0])
            );
        }

        --context->object_count;
        memset(
            &context->objects[context->object_count],
            0,
            sizeof(context->objects[0])
        );
        break;
    }

    if (
        context->object_count == 0 ||
        context->object_index >= context->object_count
    ) {
        context->object_index = 0;
    }
}


void ui_render_objects_locked(void)
{
    ui_context_t *context = ui_context();

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_MENU_OBJECTS),
        context->objects_mode == UI_OBJECTS_MENU
            ? ui_tr(UI_I18N_KEY_SHARED_FOOTER_BACK)
            : ui_tr(UI_I18N_KEY_SHARED_FOOTER_CANCEL)
    );

    if (screen == NULL) {
        return;
    }

    switch (context->objects_mode) {
        case UI_OBJECTS_SELECT_ACTIVE:
        case UI_OBJECTS_SELECT_DELETE:
        case UI_OBJECTS_SELECT_WORKFLOW:
            ui_objects_render_list(screen);
            break;

        case UI_OBJECTS_DELETE_CONFIRM:
            ui_objects_render_delete_confirm(screen);
            break;

        case UI_OBJECTS_MENU:
        default:
            ui_objects_render_menu(screen);
            break;
    }
}


void ui_object_service_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    ui_context_t *context = ui_context();

    if (
        event == NULL ||
        !atomic_load_explicit(
            &context->runtime_ready,
            memory_order_acquire
        )
    ) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    char notice[80];

    switch (event->type) {
        case OBJECT_SERVICE_EVENT_CREATE_DONE:
            ui_objects_cache_created(
                context,
                &event->object
            );

            snprintf(
                notice,
                sizeof(notice),
                ui_tr(UI_I18N_KEY_OBJECTS_CREATED),
                event->object.id
            );
            ui_set_notice_locked(notice);
            context->objects_mode = UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_LIST_DONE:
            context->objects_loaded = true;
            context->object_count = event->object_count;

            if (context->object_count > OBJECT_SERVICE_MAX_OBJECTS) {
                context->object_count = OBJECT_SERVICE_MAX_OBJECTS;
            }

            memcpy(
                context->objects,
                event->objects,
                context->object_count * sizeof(context->objects[0])
            );

            if (
                context->object_count == 0 ||
                context->object_index >= context->object_count
            ) {
                context->object_index = 0;
            }

            context->objects_mode = UI_OBJECTS_MENU;

            ui_set_notice_locked(
                context->object_count > 0
                    ? NULL
                    : ui_tr(UI_I18N_KEY_OBJECTS_NONE)
            );
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE:
            ui_objects_cache_active(
                context,
                event->object_id
            );

            snprintf(
                notice,
                sizeof(notice),
                ui_tr(UI_I18N_KEY_OBJECTS_ACTIVE_DONE),
                event->object_id
            );
            ui_set_notice_locked(notice);
            context->objects_mode = UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_DELETE_DONE:
            ui_objects_cache_deleted(
                context,
                event->object_id
            );

            snprintf(
                notice,
                sizeof(notice),
                ui_tr(UI_I18N_KEY_OBJECTS_DELETED),
                event->object_id
            );
            ui_set_notice_locked(notice);
            context->objects_mode = UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_LIST_FAILED:
            context->objects_loaded = false;
            context->object_count = 0;
            context->object_index = 0;
            context->objects_mode = UI_OBJECTS_MENU;
            ui_set_notice_locked(ui_tr(UI_I18N_KEY_OBJECTS_LIST_FAILED));
            break;

        case OBJECT_SERVICE_EVENT_CREATE_FAILED:
        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:
        case OBJECT_SERVICE_EVENT_DELETE_FAILED:
        default:
            ui_set_notice_locked(ui_tr(UI_I18N_KEY_OBJECTS_OPERATION_FAILED));
            context->objects_mode = UI_OBJECTS_MENU;
            break;
    }

    if (context->workflow_activation_pending &&
        context->workflow_object_index < context->object_count &&
        strcmp(context->objects[context->workflow_object_index].id, event->object_id) == 0 &&
        event->type == OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE) {
        context->workflow_activation_pending = false;
        ui_screen_open_locked(UI_SCREEN_WORKFLOW);
        lvgl_port_unlock();
        return;
    }

    if (event->type == OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED && context->workflow_activation_pending) {
        context->workflow_activation_pending = false;
    }

    if (context->screen == UI_SCREEN_OBJECTS) {
        ui_render_objects_locked();
    }

    lvgl_port_unlock();
}
