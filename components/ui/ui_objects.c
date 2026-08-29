#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"


#define UI_OBJECT_COLOR_TEXT     0xF3F7FB
#define UI_OBJECT_COLOR_MUTED    0x94A7BC
#define UI_OBJECT_COLOR_ACCENT   0x21C7D9
#define UI_OBJECT_COLOR_WARNING  0xF3B84B
#define UI_OBJECT_COLOR_ERROR    0xF05A67


static lv_obj_t *ui_objects_label(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    lv_obj_t *label =
        lv_label_create(
            parent
        );

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_width(
        label,
        width
    );

    lv_label_set_text(
        label,
        text
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


static void ui_objects_render_notice(
    lv_obj_t *screen
)
{
    ui_context_t *context =
        ui_context();

    if (context->notice[0] == '\0') {
        return;
    }

    ui_objects_label(
        screen,
        12,
        196,
        296,
        context->notice,
        UI_OBJECT_COLOR_WARNING
    );
}


static void ui_objects_render_menu(
    lv_obj_t *screen
)
{
    ui_context_t *context =
        ui_context();

    object_service_summary_t active;

    lv_obj_t *active_card =
        ui_create_card_locked(
            screen,
            10,
            42,
            300,
            42
        );

    if (
        object_service_get_active(
            &active
        ) == ESP_OK
    ) {
        char text[64];

        snprintf(
            text,
            sizeof(text),
            "ACTIVE  %s  %s",
            active.id,
            active.name
        );

        ui_objects_label(
            active_card,
            10,
            12,
            280,
            text,
            UI_OBJECT_COLOR_ACCENT
        );
    } else {
        ui_objects_label(
            active_card,
            10,
            12,
            280,
            "ACTIVE  NONE",
            UI_OBJECT_COLOR_MUTED
        );
    }

    static const char *actions[3] = {
        "NEW OBJECT",
        "SET ACTIVE OBJECT",
        "DELETE OBJECT",
    };

    for (size_t i = 0; i < 3; ++i) {
        ui_create_row_locked(
            screen,
            92 + (int)(i * 34U),
            actions[i],
            i == context->object_action
        );
    }

    ui_objects_render_notice(
        screen
    );
}


static void ui_objects_render_list(
    lv_obj_t *screen
)
{
    ui_context_t *context =
        ui_context();

    const char *caption =
        context->objects_mode ==
                UI_OBJECTS_SELECT_ACTIVE
            ? "SELECT OBJECT TO ACTIVATE"
            : "SELECT OBJECT TO DELETE";

    ui_objects_label(
        screen,
        12,
        42,
        296,
        caption,
        UI_OBJECT_COLOR_MUTED
    );

    if (context->object_count == 0) {
        ui_objects_label(
            screen,
            12,
            84,
            296,
            "NO OBJECTS",
            UI_OBJECT_COLOR_WARNING
        );

        ui_objects_render_notice(
            screen
        );

        return;
    }

    size_t page_start =
        (context->object_index / 4U) *
        4U;

    for (size_t row = 0; row < 4; ++row) {
        const size_t index =
            page_start + row;

        if (index >= context->object_count) {
            break;
        }

        char text[64];

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

        ui_create_row_locked(
            screen,
            62 + (int)(row * 34U),
            text,
            index == context->object_index
        );
    }

    ui_objects_render_notice(
        screen
    );
}


static void ui_objects_render_delete_confirm(
    lv_obj_t *screen
)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            16,
            62,
            288,
            112
        );

    ui_objects_label(
        card,
        14,
        14,
        260,
        "DELETE OBJECT?",
        UI_OBJECT_COLOR_ERROR
    );

    if (
        context->object_count > 0 &&
        context->object_index <
            context->object_count
    ) {
        char text[80];

        snprintf(
            text,
            sizeof(text),
            "%s  %s",
            context->objects[
                context->object_index
            ].id,
            context->objects[
                context->object_index
            ].name
        );

        ui_objects_label(
            card,
            14,
            45,
            260,
            text,
            UI_OBJECT_COLOR_TEXT
        );
    }

    ui_objects_label(
        card,
        14,
        75,
        260,
        "OK/RIGHT CONFIRM   LEFT CANCEL",
        UI_OBJECT_COLOR_WARNING
    );

    ui_objects_render_notice(
        screen
    );
}


void ui_render_objects_locked(void)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *screen =
        ui_create_screen_base_locked(
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
            ui_objects_render_list(
                screen
            );
            break;

        case UI_OBJECTS_DELETE_CONFIRM:
            ui_objects_render_delete_confirm(
                screen
            );
            break;

        case UI_OBJECTS_MENU:
        default:
            ui_objects_render_menu(
                screen
            );
            break;
    }
}


void ui_object_service_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    ui_context_t *context =
        ui_context();

    if (
        event == NULL ||
        !context->runtime_ready
    ) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    char notice[80];

    switch (event->type) {
        case OBJECT_SERVICE_EVENT_CREATE_DONE:
            snprintf(
                notice,
                sizeof(notice),
                "CREATED %s",
                event->object.id
            );

            ui_set_notice_locked(
                notice
            );

            context->objects_mode =
                UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_LIST_DONE:
            context->object_count =
                event->object_count;

            if (
                context->object_count >
                OBJECT_SERVICE_MAX_OBJECTS
            ) {
                context->object_count =
                    OBJECT_SERVICE_MAX_OBJECTS;
            }

            memcpy(
                context->objects,
                event->objects,
                context->object_count *
                    sizeof(context->objects[0])
            );

            if (
                context->object_count > 0 &&
                context->object_index >=
                    context->object_count
            ) {
                context->object_index =
                    0;
            }

            ui_set_notice_locked(
                context->object_count > 0
                    ? NULL
                    : "NO OBJECTS"
            );
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE:
            snprintf(
                notice,
                sizeof(notice),
                "ACTIVE %s",
                event->object_id
            );

            ui_set_notice_locked(
                notice
            );

            context->objects_mode =
                UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_DELETE_DONE:
            snprintf(
                notice,
                sizeof(notice),
                "DELETED %s",
                event->object_id
            );

            ui_set_notice_locked(
                notice
            );

            context->objects_mode =
                UI_OBJECTS_MENU;
            break;

        case OBJECT_SERVICE_EVENT_CREATE_FAILED:
        case OBJECT_SERVICE_EVENT_LIST_FAILED:
        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:
        case OBJECT_SERVICE_EVENT_DELETE_FAILED:
        default:
            ui_set_notice_locked(
                "OBJECT OPERATION FAILED"
            );

            context->objects_mode =
                UI_OBJECTS_MENU;
            break;
    }

    if (context->screen == UI_SCREEN_OBJECTS) {
        ui_render_objects_locked();
    }

    lvgl_port_unlock();
}
