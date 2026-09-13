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
    (OBJECT_FORMAT_ID_SIZE + OBJECT_FORMAT_NAME_SIZE + sizeof("ACTIVE    [ACTIVE]"))


static void ui_objects_render_notice(lv_obj_t *screen)
{
    ui_context_t *context = ui_context();

    if (context->notice[0] == '\0') {
        return;
    }

    (void)ui_create_label_locked(
        screen,
        12,
        196,
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
        screen, 10, 42, 300, 42
    );

    if (active_card == NULL) {
        return;
    }

    if (object_service_get_active(&active) == ESP_OK) {
        char text[UI_OBJECTS_TEXT_SIZE];
        snprintf(
            text,
            sizeof(text),
            "ACTIVE  %s  %s",
            active.id,
            active.name
        );

        (void)ui_create_label_locked(
            active_card,
            10,
            12,
            280,
            text,
            UI_COLOR_ACCENT
        );
    } else {
        (void)ui_create_label_locked(
            active_card,
            10,
            12,
            280,
            "ACTIVE  NONE",
            UI_COLOR_MUTED
        );
    }

    static const char *actions[3] = {
        "NEW OBJECT",
        "SET ACTIVE OBJECT",
        "DELETE OBJECT",
    };

    const bool have_objects =
        context->objects_loaded &&
        context->object_count > 0;

    for (size_t i = 0; i < 3; ++i) {
        const bool enabled =
            i == 0 ||
            have_objects;

        (void)ui_create_row_locked(
            screen,
            92 + (int)(i * 34U),
            actions[i],
            i == context->object_action,
            enabled
        );
    }

    ui_objects_render_notice(screen);
}


static void ui_objects_render_list(lv_obj_t *screen)
{
    ui_context_t *context = ui_context();

    const char *caption =
        context->objects_mode == UI_OBJECTS_SELECT_ACTIVE
            ? "SELECT OBJECT TO ACTIVATE"
            : "SELECT OBJECT TO DELETE";

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
            "NO OBJECTS",
            UI_COLOR_WARNING
        );
        ui_objects_render_notice(screen);
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
            "%s  %s%s",
            context->objects[index].id,
            context->objects[index].name,
            context->objects[index].active
                ? "  [ACTIVE]"
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

    ui_objects_render_notice(screen);
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
        "DELETE OBJECT?",
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
            "%s  %s",
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
        "OK/RIGHT CONFIRM   LEFT CANCEL",
        UI_COLOR_WARNING
    );

    ui_objects_render_notice(screen);
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

    for (size_t i = 0; i < context->object_count; ++i) {
        if (strcmp(context->objects[i].id, object_id) != 0) {
            continue;
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
        "OBJECTS",
        context->objects_mode == UI_OBJECTS_MENU
            ? "UP/DOWN NAV   OK SELECT   LEFT BACK"
            : "UP/DOWN NAV   OK SELECT   LEFT CANCEL"
    );

    if (screen == NULL) {
        return;
    }

    switch (context->objects_mode) {
        case UI_OBJECTS_SELECT_ACTIVE:
        case UI_OBJECTS_SELECT_DELETE:
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
                "CREATED %s",
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
                    : "NO OBJECTS"
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
                "ACTIVE %s",
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
                "DELETED %s",
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
            ui_set_notice_locked("OBJECT LIST FAILED");
            break;

        case OBJECT_SERVICE_EVENT_CREATE_FAILED:
        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:
        case OBJECT_SERVICE_EVENT_DELETE_FAILED:
        default:
            ui_set_notice_locked("OBJECT OPERATION FAILED");
            context->objects_mode = UI_OBJECTS_MENU;
            break;
    }

    if (context->screen == UI_SCREEN_OBJECTS) {
        ui_render_objects_locked();
    }

    lvgl_port_unlock();
}
