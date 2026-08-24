#include "ui_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "object_service.h"
#include "object_store.h"
#include "ui_theme.h"


static const char *TAG =
    "UI_OBJECTS";


#define OBJECT_ACTION_COUNT          2
#define OBJECT_LIST_VISIBLE          5

#define OBJECT_LIST_X                18
#define OBJECT_LIST_Y                67
#define OBJECT_LIST_WIDTH            284
#define OBJECT_LIST_HEIGHT           140
#define OBJECT_LIST_ROW_START_Y      70
#define OBJECT_LIST_ROW_STEP         28
#define OBJECT_LIST_ROW_HEIGHT       22


typedef enum
{
    OBJECT_SCREEN_MENU = 0,
    OBJECT_SCREEN_SELECT

} object_screen_mode_t;


static const char *ACTION_NAMES[
    OBJECT_ACTION_COUNT
] = {
    "NEW OBJECT",
    "SELECT OBJECT"
};


static const int ACTION_Y[
    OBJECT_ACTION_COUNT
] = {
    112,
    148
};


static object_screen_mode_t s_mode =
    OBJECT_SCREEN_MENU;


static size_t s_selected_action =
    0;


static object_store_summary_t
    s_objects[OBJECT_STORE_MAX_OBJECTS];


static size_t s_object_count =
    0;


static size_t s_selected_object =
    0;


static char s_notice[40] =
    "";


static bool s_notice_warning =
    false;


static esp_err_t ui_objects_draw_action(
    size_t action,
    bool selected
)
{
    const uint16_t foreground =
        selected
            ? DISPLAY_COLOR_BLACK
            : UI_COLOR_TEXT;


    const uint16_t background =
        selected
            ? UI_COLOR_ACCENT
            : UI_COLOR_BACKGROUND;


    ESP_ERROR_CHECK(
        display_fill_rect(
            22,
            ACTION_Y[action] - 4,
            276,
            26,
            background
        )
    );


    return display_draw_text_fast(
        34,
        ACTION_Y[action],
        ACTION_NAMES[action],
        foreground,
        background,
        2
    );
}


static esp_err_t ui_objects_draw_active_line(void)
{
    object_store_summary_t active;


    esp_err_t active_err =
        object_store_get_active(
            &active
        );


    char active_line[48];


    if (
        active_err ==
        ESP_OK
    ) {
        snprintf(
            active_line,
            sizeof(active_line),
            "ACTIVE: %s",
            active.id
        );
    }
    else {
        snprintf(
            active_line,
            sizeof(active_line),
            "ACTIVE: NONE"
        );
    }


    ESP_ERROR_CHECK(
        display_fill_rect(
            14,
            68,
            292,
            14,
            UI_COLOR_BACKGROUND
        )
    );


    return display_draw_text_fast(
        14,
        72,
        active_line,
        active_err == ESP_OK
            ? UI_COLOR_SUCCESS
            : UI_COLOR_SECONDARY,
        UI_COLOR_BACKGROUND,
        1
    );
}


static esp_err_t ui_objects_draw_notice(void)
{
    ESP_ERROR_CHECK(
        display_fill_rect(
            14,
            180,
            292,
            18,
            UI_COLOR_BACKGROUND
        )
    );


    if (
        s_notice[0] ==
        '\0'
    ) {
        return ESP_OK;
    }


    return display_draw_text_fast(
        14,
        184,
        s_notice,
        s_notice_warning
            ? UI_COLOR_WARNING
            : UI_COLOR_SUCCESS,
        UI_COLOR_BACKGROUND,
        1
    );
}


static esp_err_t ui_objects_draw_menu(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "OBJECTS"
        )
    );


    ESP_ERROR_CHECK(
        ui_objects_draw_active_line()
    );


    for (
        size_t i = 0;
        i < OBJECT_ACTION_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            ui_objects_draw_action(
                i,
                i == s_selected_action
            )
        );
    }


    ESP_ERROR_CHECK(
        ui_objects_draw_notice()
    );


    return ui_common_draw_footer(
        "UP/DOWN  OK OPEN  LEFT BACK"
    );
}


static size_t ui_objects_get_list_start(
    size_t selected_object
)
{
    size_t start =
        0;


    if (
        s_object_count >
        OBJECT_LIST_VISIBLE
    ) {
        if (
            selected_object >
            2
        ) {
            start =
                selected_object -
                2;
        }


        if (
            start +
                OBJECT_LIST_VISIBLE >
            s_object_count
        ) {
            start =
                s_object_count -
                OBJECT_LIST_VISIBLE;
        }
    }


    return start;
}


static esp_err_t ui_objects_draw_list_row(
    size_t start,
    size_t index
)
{
    if (
        index < start ||
        index >= s_object_count ||
        index >=
            start +
                OBJECT_LIST_VISIBLE
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    const size_t row =
        index -
        start;


    const bool selected =
        index ==
        s_selected_object;


    const uint16_t foreground =
        selected
            ? DISPLAY_COLOR_BLACK
            : UI_COLOR_TEXT;


    const uint16_t background =
        selected
            ? UI_COLOR_ACCENT
            : UI_COLOR_BACKGROUND;


    const int y =
        OBJECT_LIST_ROW_START_Y +
        (int)row *
            OBJECT_LIST_ROW_STEP;


    ESP_ERROR_CHECK(
        display_fill_rect(
            OBJECT_LIST_X,
            y - 3,
            OBJECT_LIST_WIDTH,
            OBJECT_LIST_ROW_HEIGHT,
            background
        )
    );


    char line[32];


    snprintf(
        line,
        sizeof(line),
        "%c %s",
        s_objects[index].active
            ? '>'
            : ' ',
        s_objects[index].id
    );


    return display_draw_text_fast(
        28,
        y,
        line,
        foreground,
        background,
        2
    );
}


static esp_err_t ui_objects_draw_list_rows(
    size_t start
)
{
    ESP_ERROR_CHECK(
        display_fill_rect(
            OBJECT_LIST_X,
            OBJECT_LIST_Y,
            OBJECT_LIST_WIDTH,
            OBJECT_LIST_HEIGHT,
            UI_COLOR_BACKGROUND
        )
    );


    size_t visible_count =
        s_object_count -
        start;


    if (
        visible_count >
        OBJECT_LIST_VISIBLE
    ) {
        visible_count =
            OBJECT_LIST_VISIBLE;
    }


    for (
        size_t row = 0;
        row < visible_count;
        ++row
    ) {
        ESP_ERROR_CHECK(
            ui_objects_draw_list_row(
                start,
                start + row
            )
        );
    }


    return ESP_OK;
}


static esp_err_t ui_objects_draw_select(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "SELECT OBJECT"
        )
    );


    if (
        s_object_count ==
        0
    ) {
        ESP_ERROR_CHECK(
            display_draw_text_fast(
                14,
                90,
                "NO OBJECTS",
                UI_COLOR_WARNING,
                UI_COLOR_BACKGROUND,
                2
            )
        );


        return ui_common_draw_footer(
            "LEFT BACK"
        );
    }


    const size_t start =
        ui_objects_get_list_start(
            s_selected_object
        );


    ESP_ERROR_CHECK(
        ui_objects_draw_list_rows(
            start
        )
    );


    return ui_common_draw_footer(
        "UP/DOWN SELECT  OK SET  LEFT BACK"
    );
}


static esp_err_t ui_objects_update_list_selection(
    size_t previous_object
)
{
    const size_t previous_start =
        ui_objects_get_list_start(
            previous_object
        );


    const size_t new_start =
        ui_objects_get_list_start(
            s_selected_object
        );


    if (
        previous_start !=
        new_start
    ) {
        return ui_objects_draw_list_rows(
            new_start
        );
    }


    ESP_ERROR_CHECK(
        ui_objects_draw_list_row(
            new_start,
            previous_object
        )
    );


    return ui_objects_draw_list_row(
        new_start,
        s_selected_object
    );
}


static void ui_objects_select_active_item(void)
{
    s_selected_object =
        0;


    for (
        size_t i = 0;
        i < s_object_count;
        ++i
    ) {
        if (
            s_objects[i].active
        ) {
            s_selected_object =
                i;


            return;
        }
    }
}


static void ui_objects_report_draw_error(
    esp_err_t err
)
{
    if (
        err ==
        ESP_OK
    ) {
        return;
    }


    ESP_LOGE(
        TAG,
        "Background redraw failed: %s",
        esp_err_to_name(err)
    );
}


static void ui_objects_service_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;


    if (
        event ==
        NULL
    ) {
        return;
    }


    esp_err_t draw_err =
        ESP_OK;


    switch (
        event->type
    ) {

        case OBJECT_SERVICE_EVENT_CREATE_DONE:

            snprintf(
                s_notice,
                sizeof(s_notice),
                "CREATED %s",
                event->object.id
            );


            s_notice_warning =
                false;


            draw_err =
                ui_objects_draw_active_line();


            if (
                draw_err ==
                ESP_OK
            ) {
                draw_err =
                    ui_objects_draw_notice();
            }


            break;


        case OBJECT_SERVICE_EVENT_CREATE_FAILED:

            snprintf(
                s_notice,
                sizeof(s_notice),
                "CREATE FAILED"
            );


            s_notice_warning =
                true;


            draw_err =
                ui_objects_draw_notice();


            break;


        case OBJECT_SERVICE_EVENT_LIST_DONE:

            s_object_count =
                event->object_count;


            if (
                s_object_count >
                OBJECT_STORE_MAX_OBJECTS
            ) {
                s_object_count =
                    OBJECT_STORE_MAX_OBJECTS;
            }


            memcpy(
                s_objects,
                event->objects,
                s_object_count *
                    sizeof(
                        object_store_summary_t
                    )
            );


            ui_objects_select_active_item();


            if (
                s_object_count ==
                0
            ) {
                snprintf(
                    s_notice,
                    sizeof(s_notice),
                    "NO OBJECTS"
                );


                s_notice_warning =
                    true;


                s_mode =
                    OBJECT_SCREEN_MENU;


                draw_err =
                    ui_objects_draw_menu();
            }
            else {
                s_notice[0] =
                    '\0';


                s_mode =
                    OBJECT_SCREEN_SELECT;


                draw_err =
                    ui_objects_draw_select();
            }


            break;


        case OBJECT_SERVICE_EVENT_LIST_FAILED:

            snprintf(
                s_notice,
                sizeof(s_notice),
                "LOAD FAILED"
            );


            s_notice_warning =
                true;


            s_mode =
                OBJECT_SCREEN_MENU;


            draw_err =
                ui_objects_draw_menu();


            break;


        case OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE:

            snprintf(
                s_notice,
                sizeof(s_notice),
                "ACTIVE %s",
                event->object_id
            );


            s_notice_warning =
                false;


            s_mode =
                OBJECT_SCREEN_MENU;


            draw_err =
                ui_objects_draw_menu();


            break;


        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:

            draw_err =
                ui_common_draw_footer(
                    "SAVE FAILED  LEFT BACK"
                );


            break;


        default:

            break;
    }


    ui_objects_report_draw_error(
        draw_err
    );
}


static esp_err_t ui_objects_create(void)
{
    snprintf(
        s_notice,
        sizeof(s_notice),
        "CREATING..."
    );


    s_notice_warning =
        false;


    ESP_ERROR_CHECK(
        ui_objects_draw_notice()
    );


    esp_err_t err =
        object_service_create_async();


    if (
        err !=
        ESP_OK
    ) {
        snprintf(
            s_notice,
            sizeof(s_notice),
            "CREATE FAILED"
        );


        s_notice_warning =
            true;


        ui_objects_draw_notice();


        return err;
    }


    return ESP_OK;
}


static esp_err_t ui_objects_enter_select(void)
{
    snprintf(
        s_notice,
        sizeof(s_notice),
        "LOADING..."
    );


    s_notice_warning =
        false;


    ESP_ERROR_CHECK(
        ui_objects_draw_notice()
    );


    esp_err_t err =
        object_service_list_async();


    if (
        err !=
        ESP_OK
    ) {
        snprintf(
            s_notice,
            sizeof(s_notice),
            "LOAD FAILED"
        );


        s_notice_warning =
            true;


        ui_objects_draw_notice();


        return err;
    }


    return ESP_OK;
}


esp_err_t ui_objects_open(void)
{
    esp_err_t err =
        object_service_set_event_callback(
            ui_objects_service_event,
            NULL
        );


    if (
        err !=
        ESP_OK
    ) {
        return err;
    }


    s_mode =
        OBJECT_SCREEN_MENU;


    s_selected_action =
        0;


    s_notice[0] =
        '\0';


    return ui_objects_draw_menu();
}


esp_err_t ui_objects_draw(void)
{
    if (
        s_mode ==
        OBJECT_SCREEN_SELECT
    ) {
        return ui_objects_draw_select();
    }


    return ui_objects_draw_menu();
}


esp_err_t ui_objects_handle_key(
    keypad_key_t key
)
{
    /*
     * Storage worker may be writing metadata or scanning the card.
     *
     * Ignore navigation while the worker is active so the worker
     * completion callback and keypad task never draw concurrently.
     */
    if (
        object_service_is_busy()
    ) {
        return ESP_OK;
    }


    if (
        s_mode ==
        OBJECT_SCREEN_SELECT
    ) {
        switch (
            key
        ) {

            case KEYPAD_KEY_UP:

                if (
                    s_object_count >
                    0
                ) {
                    const size_t previous =
                        s_selected_object;


                    if (
                        s_selected_object ==
                        0
                    ) {
                        s_selected_object =
                            s_object_count -
                            1;
                    }
                    else {
                        s_selected_object--;
                    }


                    return
                        ui_objects_update_list_selection(
                            previous
                        );
                }


                return ESP_OK;


            case KEYPAD_KEY_DOWN:

                if (
                    s_object_count >
                    0
                ) {
                    const size_t previous =
                        s_selected_object;


                    s_selected_object++;


                    if (
                        s_selected_object >=
                        s_object_count
                    ) {
                        s_selected_object =
                            0;
                    }


                    return
                        ui_objects_update_list_selection(
                            previous
                        );
                }


                return ESP_OK;


            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_RIGHT:

                if (
                    s_object_count ==
                    0
                ) {
                    return ESP_OK;
                }


                ESP_ERROR_CHECK(
                    ui_common_draw_footer(
                        "SAVING ACTIVE OBJECT..."
                    )
                );


                {
                    esp_err_t err =
                        object_service_set_active_async(
                            s_objects[
                                s_selected_object
                            ].id
                        );


                    if (
                        err !=
                        ESP_OK
                    ) {
                        ui_common_draw_footer(
                            "SAVE FAILED  LEFT BACK"
                        );


                        return err;
                    }
                }


                return ESP_OK;


            case KEYPAD_KEY_LEFT:

                s_mode =
                    OBJECT_SCREEN_MENU;


                return ui_objects_draw_menu();


            case KEYPAD_KEY_NONE:
            default:

                return ESP_OK;
        }
    }


    switch (
        key
    ) {

        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:

            {
                const size_t previous =
                    s_selected_action;


                s_selected_action =
                    s_selected_action ==
                            0
                        ? 1
                        : 0;


                ESP_ERROR_CHECK(
                    ui_objects_draw_action(
                        previous,
                        false
                    )
                );


                return ui_objects_draw_action(
                    s_selected_action,
                    true
                );
            }


        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:

            if (
                s_selected_action ==
                0
            ) {
                return ui_objects_create();
            }


            return ui_objects_enter_select();


        case KEYPAD_KEY_LEFT:

            return ESP_ERR_NOT_SUPPORTED;


        case KEYPAD_KEY_NONE:
        default:

            return ESP_OK;
    }
}