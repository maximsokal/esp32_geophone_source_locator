#include "ui.h"

#include <stddef.h>

#include "esp_err.h"
#include "esp_log.h"

#include "ui_internal.h"


static const char *TAG =
    "UI";


static ui_screen_t s_screen =
    UI_SCREEN_HOME;


static size_t s_selected_item =
    0;


static esp_err_t ui_draw_current_screen(void)
{
    switch (s_screen) {
        case UI_SCREEN_OBJECTS:
            return ui_objects_draw();

        case UI_SCREEN_MEASUREMENTS:
            return ui_measurements_draw();

        case UI_SCREEN_STORAGE:
            return ui_storage_draw();

        case UI_SCREEN_WIFI:
            return ui_wifi_draw();

        case UI_SCREEN_SYSTEM:
            return ui_system_draw();

        case UI_SCREEN_HOME:
        default:
            return ui_home_draw(
                s_selected_item
            );
    }
}


static esp_err_t ui_open_selected_item(void)
{
    switch (s_selected_item) {
        case 0:
            s_screen =
                UI_SCREEN_OBJECTS;

            return ui_objects_open();

        case 1:
            s_screen =
                UI_SCREEN_MEASUREMENTS;
            break;

        case 2:
            s_screen =
                UI_SCREEN_STORAGE;

            return ui_storage_open();

        case 3:
            s_screen =
                UI_SCREEN_WIFI;
            break;

        case 4:
            s_screen =
                UI_SCREEN_SYSTEM;
            break;

        default:
            return ESP_ERR_INVALID_STATE;
    }

    return ui_draw_current_screen();
}


static esp_err_t ui_select_previous_item(void)
{
    size_t previous =
        s_selected_item;

    if (s_selected_item == 0) {
        s_selected_item =
            UI_MENU_ITEM_COUNT - 1;
    }
    else {
        s_selected_item--;
    }

    return ui_home_update_selection(
        previous,
        s_selected_item
    );
}


static esp_err_t ui_select_next_item(void)
{
    size_t previous =
        s_selected_item;

    s_selected_item++;

    if (
        s_selected_item >=
        UI_MENU_ITEM_COUNT
    ) {
        s_selected_item =
            0;
    }

    return ui_home_update_selection(
        previous,
        s_selected_item
    );
}


static esp_err_t ui_return_home(void)
{
    s_screen =
        UI_SCREEN_HOME;

    return ui_home_draw(
        s_selected_item
    );
}


esp_err_t ui_init(void)
{
    s_screen =
        UI_SCREEN_HOME;

    s_selected_item =
        0;

    ESP_LOGI(
        TAG,
        "Initializing UI"
    );

    esp_err_t err =
        ui_draw_current_screen();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize UI: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "UI ready"
    );

    return ESP_OK;
}


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
)
{
    (void)user_context;

    ESP_LOGI(
        TAG,
        "UI key: %s",
        keypad_key_name(key)
    );

    esp_err_t err =
        ESP_OK;

    if (
        s_screen ==
        UI_SCREEN_HOME
    ) {
        switch (key) {
            case KEYPAD_KEY_UP:
                err =
                    ui_select_previous_item();
                break;

            case KEYPAD_KEY_DOWN:
                err =
                    ui_select_next_item();
                break;

            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_RIGHT:
                err =
                    ui_open_selected_item();
                break;

            case KEYPAD_KEY_LEFT:
            case KEYPAD_KEY_NONE:
            default:
                return;
        }

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Home navigation failed: %s",
                esp_err_to_name(err)
            );
        }

        return;
    }

    if (
        s_screen ==
        UI_SCREEN_OBJECTS
    ) {
        err =
            ui_objects_handle_key(
                key
            );

        if (err == ESP_OK) {
            return;
        }

        if (
            err !=
            ESP_ERR_NOT_SUPPORTED
        ) {
            ESP_LOGE(
                TAG,
                "Objects navigation failed: %s",
                esp_err_to_name(err)
            );

            return;
        }
    }

    if (
        s_screen ==
        UI_SCREEN_STORAGE
    ) {
        err =
            ui_storage_handle_key(
                key
            );

        if (err == ESP_OK) {
            return;
        }

        if (
            err !=
            ESP_ERR_NOT_SUPPORTED
        ) {
            ESP_LOGE(
                TAG,
                "Storage navigation failed: %s",
                esp_err_to_name(err)
            );

            return;
        }
    }

    if (
        key ==
        KEYPAD_KEY_LEFT
    ) {
        err =
            ui_return_home();

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to return home: %s",
                esp_err_to_name(err)
            );
        }
    }
}