#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "app_event_bus.h"
#include "wifi_manager.h"


#define UI_COLOR_BACKGROUND  0x08111F
#define UI_COLOR_HEADER      0x0E1A2B
#define UI_COLOR_PANEL       0x132238
#define UI_COLOR_PANEL_ALT   0x1B2D46
#define UI_COLOR_ACCENT      0x21C7D9
#define UI_COLOR_GOOD        0x4CD681
#define UI_COLOR_WARNING     0xF3B84B
#define UI_COLOR_TEXT        0xF3F7FB
#define UI_COLOR_MUTED       0x94A7BC


static void ui_style_screen(
    lv_obj_t *screen
)
{
    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(UI_COLOR_BACKGROUND),
        0
    );

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_width(
        screen,
        0,
        0
    );

    lv_obj_set_style_pad_all(
        screen,
        0,
        0
    );

    lv_obj_set_style_text_color(
        screen,
        lv_color_hex(UI_COLOR_TEXT),
        0
    );
}


static lv_obj_t *ui_create_label(
    lv_obj_t *parent,
    int x,
    int y,
    const char *text,
    uint32_t color
)
{
    lv_obj_t *label =
        lv_label_create(
            parent
        );

    lv_label_set_text(
        label,
        text != NULL
            ? text
            : ""
    );

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


lv_obj_t *ui_create_screen_base_locked(
    const char *title,
    const char *footer
)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *previous =
        context->root;

    context->header_source =
        NULL;

    context->header_wifi =
        NULL;

    memset(
        context->home_rows,
        0,
        sizeof(context->home_rows)
    );

    memset(
        context->measurement_channel_labels,
        0,
        sizeof(context->measurement_channel_labels)
    );

    context->measurement_summary =
        NULL;

    context->measurement_transport =
        NULL;

    context->measurement_health =
        NULL;

    lv_obj_t *screen =
        lv_obj_create(
            NULL
        );

    if (screen == NULL) {
        return NULL;
    }

    ui_style_screen(
        screen
    );

    context->root =
        screen;

    lv_obj_t *header =
        lv_obj_create(
            screen
        );

    lv_obj_set_pos(
        header,
        0,
        0
    );

    lv_obj_set_size(
        header,
        320,
        34
    );

    lv_obj_set_style_bg_color(
        header,
        lv_color_hex(UI_COLOR_HEADER),
        0
    );

    lv_obj_set_style_bg_opa(
        header,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_width(
        header,
        0,
        0
    );

    lv_obj_set_style_radius(
        header,
        0,
        0
    );

    lv_obj_set_style_pad_all(
        header,
        0,
        0
    );

    lv_obj_t *title_label =
        ui_create_label(
            header,
            10,
            9,
            title,
            UI_COLOR_TEXT
        );

    lv_obj_set_width(
        title_label,
        112
    );

    context->header_source =
        ui_create_label(
            header,
            128,
            9,
            "ADC --",
            UI_COLOR_ACCENT
        );

    lv_obj_set_width(
        context->header_source,
        112
    );

    context->header_wifi =
        ui_create_label(
            header,
            248,
            9,
            "NET --",
            UI_COLOR_MUTED
        );

    lv_obj_set_width(
        context->header_wifi,
        66
    );

    lv_obj_set_style_text_align(
        context->header_wifi,
        LV_TEXT_ALIGN_RIGHT,
        0
    );

    lv_obj_t *footer_bar =
        lv_obj_create(
            screen
        );

    lv_obj_set_pos(
        footer_bar,
        0,
        216
    );

    lv_obj_set_size(
        footer_bar,
        320,
        24
    );

    lv_obj_set_style_bg_color(
        footer_bar,
        lv_color_hex(UI_COLOR_HEADER),
        0
    );

    lv_obj_set_style_bg_opa(
        footer_bar,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_width(
        footer_bar,
        0,
        0
    );

    lv_obj_set_style_radius(
        footer_bar,
        0,
        0
    );

    lv_obj_set_style_pad_all(
        footer_bar,
        0,
        0
    );

    lv_obj_t *footer_label =
        ui_create_label(
            footer_bar,
            8,
            6,
            footer,
            UI_COLOR_MUTED
        );

    lv_obj_set_width(
        footer_label,
        304
    );

    ui_refresh_header_locked();

    lv_screen_load(
        screen
    );

    if (
        previous != NULL &&
        previous != screen &&
        lv_obj_is_valid(previous)
    ) {
        lv_obj_delete(
            previous
        );
    }

    return screen;
}


lv_obj_t *ui_create_card_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    int height
)
{
    lv_obj_t *card =
        lv_obj_create(
            parent
        );

    lv_obj_set_pos(
        card,
        x,
        y
    );

    lv_obj_set_size(
        card,
        width,
        height
    );

    lv_obj_set_style_bg_color(
        card,
        lv_color_hex(UI_COLOR_PANEL),
        0
    );

    lv_obj_set_style_bg_opa(
        card,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_color(
        card,
        lv_color_hex(UI_COLOR_PANEL_ALT),
        0
    );

    lv_obj_set_style_border_width(
        card,
        1,
        0
    );

    lv_obj_set_style_radius(
        card,
        7,
        0
    );

    lv_obj_set_style_pad_all(
        card,
        0,
        0
    );

    return card;
}


lv_obj_t *ui_create_row_locked(
    lv_obj_t *parent,
    int y,
    const char *text,
    bool selected
)
{
    lv_obj_t *row =
        ui_create_card_locked(
            parent,
            10,
            y,
            300,
            31
        );

    if (selected) {
        lv_obj_set_style_bg_color(
            row,
            lv_color_hex(UI_COLOR_PANEL_ALT),
            0
        );

        lv_obj_set_style_border_color(
            row,
            lv_color_hex(UI_COLOR_ACCENT),
            0
        );

        lv_obj_set_style_border_width(
            row,
            2,
            0
        );
    }

    ui_create_label(
        row,
        10,
        8,
        text,
        selected
            ? UI_COLOR_TEXT
            : UI_COLOR_MUTED
    );

    if (selected) {
        ui_create_label(
            row,
            278,
            8,
            ">",
            UI_COLOR_ACCENT
        );
    }

    return row;
}


void ui_set_notice_locked(
    const char *text
)
{
    ui_context_t *context =
        ui_context();

    if (text == NULL) {
        context->notice[0] =
            '\0';

        return;
    }

    snprintf(
        context->notice,
        sizeof(context->notice),
        "%s",
        text
    );
}


void ui_refresh_header_locked(void)
{
    ui_context_t *context =
        ui_context();

    if (
        context->header_source == NULL ||
        context->header_wifi == NULL
    ) {
        return;
    }

    seismic_acquisition_snapshot_t acquisition;

    if (
        seismic_acquisition_get_snapshot(
            &acquisition
        ) == ESP_OK
    ) {
        char text[32];

        snprintf(
            text,
            sizeof(text),
            "%s %s",
            acquisition.source_name,
            acquisition.running
                ? "RUN"
                : "OFF"
        );

        lv_label_set_text(
            context->header_source,
            text
        );

        lv_obj_set_style_text_color(
            context->header_source,
            lv_color_hex(
                acquisition.running
                    ? UI_COLOR_GOOD
                    : UI_COLOR_WARNING
            ),
            0
        );
    }

    char ip[24] = {0};

    if (wifi_manager_is_setup_mode()) {
        lv_label_set_text(
            context->header_wifi,
            "SETUP"
        );

        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_WARNING),
            0
        );
    } else if (
        wifi_manager_sta_ip_string(
            ip,
            sizeof(ip)
        ) == ESP_OK &&
        ip[0] != '\0'
    ) {
        lv_label_set_text(
            context->header_wifi,
            "NET OK"
        );

        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_GOOD),
            0
        );
    } else {
        lv_label_set_text(
            context->header_wifi,
            "NET --"
        );

        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_MUTED),
            0
        );
    }
}


void ui_render_boot_locked(
    const char *stage
)
{
    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "GEOPHONE LOCATOR",
            "ESP32-S3  |  INITIALIZING"
        );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            18,
            58,
            284,
            116
        );

    ui_create_label(
        card,
        18,
        18,
        "SYSTEM STARTUP",
        UI_COLOR_ACCENT
    );

    ui_create_label(
        card,
        18,
        51,
        stage,
        UI_COLOR_TEXT
    );

    lv_obj_t *bar =
        lv_bar_create(
            card
        );

    lv_obj_set_pos(
        bar,
        18,
        82
    );

    lv_obj_set_size(
        bar,
        248,
        10
    );

    lv_bar_set_range(
        bar,
        0,
        100
    );

    int value =
        12;

    if (stage != NULL) {
        if (strstr(stage, "STORAGE") != NULL) {
            value = 35;
        } else if (strstr(stage, "RECOVERY") != NULL) {
            value = 55;
        } else if (strstr(stage, "OBJECT") != NULL) {
            value = 70;
        } else if (strstr(stage, "NETWORK") != NULL) {
            value = 88;
        }
    }

    lv_bar_set_value(
        bar,
        value,
        LV_ANIM_OFF
    );

    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_PANEL_ALT),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_ACCENT),
        LV_PART_INDICATOR
    );
}


void ui_render_current_locked(void)
{
    switch (ui_context()->screen) {
        case UI_SCREEN_OBJECTS:
            ui_render_objects_locked();
            break;

        case UI_SCREEN_MEASUREMENTS:
            ui_render_measurements_locked();
            break;

        case UI_SCREEN_STORAGE:
            ui_render_storage_locked();
            break;

        case UI_SCREEN_WIFI:
            ui_render_wifi_locked();
            break;

        case UI_SCREEN_SYSTEM:
            ui_render_system_locked();
            break;

        case UI_SCREEN_HOME:
        default:
            ui_render_home_locked();
            break;
    }
}


void ui_refresh_dynamic_locked(void)
{
    ui_refresh_header_locked();

    if (
        ui_context()->screen ==
        UI_SCREEN_MEASUREMENTS
    ) {
        ui_measurements_refresh_locked();
    }
}


static void ui_open_home_locked(void)
{
    ui_context_t *context =
        ui_context();

    context->screen =
        UI_SCREEN_HOME;

    context->selected =
        0;

    context->storage_erase_confirm =
        false;

    ui_set_notice_locked(
        NULL
    );

    ui_render_home_locked();
}


static void ui_open_home_selection_locked(void)
{
    ui_context_t *context =
        ui_context();

    static const ui_screen_t screens[5] = {
        UI_SCREEN_OBJECTS,
        UI_SCREEN_MEASUREMENTS,
        UI_SCREEN_STORAGE,
        UI_SCREEN_WIFI,
        UI_SCREEN_SYSTEM,
    };

    if (context->selected >= 5) {
        context->selected =
            0;
    }

    context->screen =
        screens[context->selected];

    context->selected =
        0;

    ui_set_notice_locked(
        NULL
    );

    if (context->screen == UI_SCREEN_OBJECTS) {
        context->objects_mode =
            UI_OBJECTS_MENU;

        context->object_action =
            0;
    } else if (context->screen == UI_SCREEN_STORAGE) {
        context->storage_action =
            0;

        context->storage_erase_confirm =
            false;

        (void)storage_service_info_async();
    } else if (context->screen == UI_SCREEN_WIFI) {
        context->wifi_action =
            0;
    }

    ui_render_current_locked();
}


static void ui_move_index(
    size_t *value,
    size_t count,
    bool down
)
{
    if (
        value == NULL ||
        count == 0
    ) {
        return;
    }

    if (down) {
        *value =
            (*value + 1U) %
            count;
    } else if (*value == 0) {
        *value =
            count - 1U;
    } else {
        (*value)--;
    }
}


static void ui_handle_objects_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (object_service_is_busy()) {
        return;
    }

    if (context->objects_mode == UI_OBJECTS_MENU) {
        if (key == KEYPAD_KEY_UP) {
            ui_move_index(
                &context->object_action,
                3,
                false
            );

            ui_render_objects_locked();
        } else if (key == KEYPAD_KEY_DOWN) {
            ui_move_index(
                &context->object_action,
                3,
                true
            );

            ui_render_objects_locked();
        } else if (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        ) {
            esp_err_t err =
                ESP_OK;

            if (context->object_action == 0) {
                ui_set_notice_locked(
                    "CREATING OBJECT..."
                );

                err =
                    object_service_create_async();
            } else {
                context->objects_mode =
                    context->object_action == 1
                        ? UI_OBJECTS_SELECT_ACTIVE
                        : UI_OBJECTS_SELECT_DELETE;

                context->object_index =
                    0;

                ui_set_notice_locked(
                    "LOADING OBJECTS..."
                );

                err =
                    object_service_list_async();
            }

            if (err != ESP_OK) {
                ui_set_notice_locked(
                    "OBJECT SERVICE BUSY/FAILED"
                );
            }

            ui_render_objects_locked();
        } else if (key == KEYPAD_KEY_LEFT) {
            ui_open_home_locked();
        }

        return;
    }

    if (context->objects_mode == UI_OBJECTS_DELETE_CONFIRM) {
        if (key == KEYPAD_KEY_LEFT) {
            context->objects_mode =
                UI_OBJECTS_SELECT_DELETE;

            ui_render_objects_locked();
        } else if (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        ) {
            if (
                context->object_count > 0 &&
                context->object_index <
                    context->object_count
            ) {
                esp_err_t err =
                    object_service_delete_async(
                        context->objects[
                            context->object_index
                        ].id
                    );

                ui_set_notice_locked(
                    err == ESP_OK
                        ? "DELETING..."
                        : "DELETE FAILED TO START"
                );

                ui_render_objects_locked();
            }
        }

        return;
    }

    if (key == KEYPAD_KEY_LEFT) {
        context->objects_mode =
            UI_OBJECTS_MENU;

        context->object_action =
            0;

        ui_render_objects_locked();

        return;
    }

    if (context->object_count == 0) {
        return;
    }

    if (key == KEYPAD_KEY_UP) {
        ui_move_index(
            &context->object_index,
            context->object_count,
            false
        );

        ui_render_objects_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(
            &context->object_index,
            context->object_count,
            true
        );

        ui_render_objects_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        if (
            context->object_index >=
            context->object_count
        ) {
            return;
        }

        if (
            context->objects_mode ==
            UI_OBJECTS_SELECT_ACTIVE
        ) {
            esp_err_t err =
                object_service_set_active_async(
                    context->objects[
                        context->object_index
                    ].id
                );

            ui_set_notice_locked(
                err == ESP_OK
                    ? "SETTING ACTIVE..."
                    : "SET ACTIVE FAILED TO START"
            );

            ui_render_objects_locked();
        } else {
            context->objects_mode =
                UI_OBJECTS_DELETE_CONFIRM;

            ui_render_objects_locked();
        }
    }
}


static void ui_handle_storage_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (storage_service_is_busy()) {
        return;
    }

    if (key == KEYPAD_KEY_LEFT) {
        context->storage_erase_confirm =
            false;

        ui_open_home_locked();
    } else if (key == KEYPAD_KEY_UP) {
        context->storage_erase_confirm =
            false;

        ui_move_index(
            &context->storage_action,
            2,
            false
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_storage_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        context->storage_erase_confirm =
            false;

        ui_move_index(
            &context->storage_action,
            2,
            true
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_storage_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        if (context->storage_action == 0) {
            context->storage_erase_confirm =
                false;

            ui_set_notice_locked(
                "REFRESHING SD INFO..."
            );

            esp_err_t err =
                storage_service_info_async();

            if (err != ESP_OK) {
                ui_set_notice_locked(
                    "STORAGE SERVICE BUSY/FAILED"
                );
            }
        } else if (!context->storage_erase_confirm) {
            context->storage_erase_confirm =
                true;

            ui_set_notice_locked(
                "PRESS OK AGAIN TO ERASE DATA"
            );
        } else {
            context->storage_erase_confirm =
                false;

            ui_set_notice_locked(
                "ERASING DATA..."
            );

            esp_err_t err =
                storage_service_erase_data_async();

            if (err != ESP_OK) {
                ui_set_notice_locked(
                    "STORAGE SERVICE BUSY/FAILED"
                );
            }
        }

        ui_render_storage_locked();
    }
}


static void ui_handle_wifi_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (key == KEYPAD_KEY_LEFT) {
        ui_open_home_locked();
    } else if (key == KEYPAD_KEY_UP) {
        ui_move_index(
            &context->wifi_action,
            3,
            false
        );

        ui_render_wifi_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(
            &context->wifi_action,
            3,
            true
        );

        ui_render_wifi_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        esp_err_t err;

        if (context->wifi_action == 0) {
            err =
                wifi_manager_enter_setup_mode();
        } else if (context->wifi_action == 1) {
            err =
                wifi_manager_connect_saved();
        } else {
            err =
                wifi_manager_forget_sta_credentials();
        }

        ui_set_notice_locked(
            err == ESP_OK
                ? "WI-FI COMMAND ACCEPTED"
                : "WI-FI COMMAND FAILED"
        );

        ui_render_wifi_locked();
    }
}


void ui_dispatch_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    switch (context->screen) {
        case UI_SCREEN_HOME:
            if (key == KEYPAD_KEY_UP) {
                ui_move_index(
                    &context->selected,
                    5,
                    false
                );

                ui_render_home_locked();
            } else if (key == KEYPAD_KEY_DOWN) {
                ui_move_index(
                    &context->selected,
                    5,
                    true
                );

                ui_render_home_locked();
            } else if (
                key == KEYPAD_KEY_OK ||
                key == KEYPAD_KEY_RIGHT
            ) {
                ui_open_home_selection_locked();
            }
            break;

        case UI_SCREEN_OBJECTS:
            ui_handle_objects_key_locked(
                key
            );
            break;

        case UI_SCREEN_STORAGE:
            ui_handle_storage_key_locked(
                key
            );
            break;

        case UI_SCREEN_WIFI:
            ui_handle_wifi_key_locked(
                key
            );
            break;

        case UI_SCREEN_MEASUREMENTS:
        case UI_SCREEN_SYSTEM:
            if (key == KEYPAD_KEY_LEFT) {
                ui_open_home_locked();
            }
            break;

        default:
            break;
    }
}


void ui_dispatch_app_event_locked(
    int event_type
)
{
    switch ((app_event_type_t)event_type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
        case APP_EVENT_WIFI_STA_CONNECTED:
        case APP_EVENT_WIFI_STA_DISCONNECTED:
            ui_refresh_header_locked();

            if (
                ui_context()->screen ==
                UI_SCREEN_WIFI
            ) {
                ui_render_wifi_locked();
            }
            break;

        case APP_EVENT_KEYPAD:
        default:
            break;
    }
}
