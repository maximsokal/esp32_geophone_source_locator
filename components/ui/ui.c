#include "ui.h"

#include <stddef.h>

#include "esp_log.h"

#include "ui_internal.h"


static const char *TAG = "UI";


static ui_screen_t s_screen =
    UI_SCREEN_HOME;

static size_t s_selected_item = 0;


static esp_err_t ui_draw_current_screen(void)
{
    switch (s_screen) {

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


static void ui_open_selected_item(void)
{
    switch (s_selected_item) {

        case 0:
            s_screen =
                UI_SCREEN_MEASUREMENTS;
            break;

        case 1:
            s_screen =
                UI_SCREEN_STORAGE;
            break;

        case 2:
            s_screen =
                UI_SCREEN_WIFI;
            break;

        case 3:
            s_screen =
                UI_SCREEN_SYSTEM;
            break;

        default:
            s_screen =
                UI_SCREEN_HOME;
            break;
    }


    esp_err_t err =
        ui_draw_current_screen();


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to open screen: %s",
            esp_err_to_name(err)
        );
    }
}


esp_err_t ui_init(void)
{
    s_screen = UI_SCREEN_HOME;
    s_selected_item = 0;

    ESP_LOGI(
        TAG,
        "Initializing UI"
    );


    esp_err_t err =
        ui_draw_current_screen();


    if (err == ESP_OK) {

        ESP_LOGI(
            TAG,
            "UI ready"
        );
    }


    return err;
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


    if (s_screen == UI_SCREEN_HOME) {

        switch (key) {

            case KEYPAD_KEY_UP:

                if (s_selected_item == 0) {

                    s_selected_item =
                        UI_MENU_ITEM_COUNT - 1;
                }
                else {

                    s_selected_item--;
                }

                break;


            case KEYPAD_KEY_DOWN:

                s_selected_item++;

                if (
                    s_selected_item >=
                    UI_MENU_ITEM_COUNT
                ) {

                    s_selected_item = 0;
                }

                break;


            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_RIGHT:

                ui_open_selected_item();
                return;


            case KEYPAD_KEY_LEFT:
            case KEYPAD_KEY_NONE:
            default:

                return;
        }


        esp_err_t err =
            ui_home_draw(
                s_selected_item
            );


        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to redraw home screen: %s",
                esp_err_to_name(err)
            );
        }


        return;
    }


    /*
     * LEFT always returns from a child screen
     * to the main menu.
     */
    if (key == KEYPAD_KEY_LEFT) {

        s_screen = UI_SCREEN_HOME;

        esp_err_t err =
            ui_home_draw(
                s_selected_item
            );


        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to return home: %s",
                esp_err_to_name(err)
            );
        }
    }
}