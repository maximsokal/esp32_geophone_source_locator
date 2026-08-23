#include "ui.h"

#include <stddef.h>

#include "display.h"
#include "esp_log.h"


static const char *TAG = "UI";


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_DETAIL
} ui_screen_t;


static const char *MENU_ITEMS[] = {
    "MEASUREMENTS",
    "STORAGE",
    "WI-FI",
    "SYSTEM"
};


#define MENU_ITEM_COUNT \
    (sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]))


static const int MENU_Y[] = {
    48,
    72,
    96,
    120
};


static ui_screen_t s_screen = UI_SCREEN_HOME;
static size_t s_selected_item = 0;


static esp_err_t ui_draw_header(void)
{
    ESP_ERROR_CHECK(
        display_fill_rect(
            0,
            0,
            DISPLAY_WIDTH,
            34,
            DISPLAY_COLOR_NAVY
        )
    );

    return display_draw_text(
        10,
        8,
        "GEO LOCATOR",
        DISPLAY_COLOR_WHITE,
        DISPLAY_COLOR_NAVY,
        2
    );
}


static esp_err_t ui_draw_menu(void)
{
    /*
     * Clear only the menu area so navigation does not redraw
     * the complete screen.
     */
    ESP_ERROR_CHECK(
        display_fill_rect(
            158,
            40,
            162,
            150,
            DISPLAY_COLOR_BLACK
        )
    );

    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        const bool selected =
            i == s_selected_item;

        uint16_t background =
            selected
                ? DISPLAY_COLOR_CYAN
                : DISPLAY_COLOR_BLACK;

        uint16_t foreground =
            selected
                ? DISPLAY_COLOR_BLACK
                : DISPLAY_COLOR_WHITE;

        if (selected) {
            ESP_ERROR_CHECK(
                display_fill_rect(
                    164,
                    MENU_Y[i] - 4,
                    148,
                    17,
                    background
                )
            );
        }

        ESP_ERROR_CHECK(
            display_draw_text(
                170,
                MENU_Y[i],
                MENU_ITEMS[i],
                foreground,
                background,
                1
            )
        );
    }

    return ESP_OK;
}


static esp_err_t ui_draw_home(void)
{
    ESP_ERROR_CHECK(
        display_fill(DISPLAY_COLOR_BLACK)
    );

    ESP_ERROR_CHECK(ui_draw_header());

    /*
     * Device status section.
     */
    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            48,
            "SYSTEM",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            78,
            48,
            "READY",
            DISPLAY_COLOR_GREEN,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            66,
            "SD CARD",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            78,
            66,
            "OK",
            DISPLAY_COLOR_GREEN,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            84,
            "WI-FI",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            78,
            84,
            "OFF",
            DISPLAY_COLOR_YELLOW,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            102,
            "SENSORS",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            78,
            102,
            "--",
            DISPLAY_COLOR_YELLOW,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    /*
     * Vertical separator between status and navigation.
     */
    ESP_ERROR_CHECK(
        display_fill_rect(
            151,
            44,
            1,
            142,
            DISPLAY_COLOR_DARK_GRAY
        )
    );

    ESP_ERROR_CHECK(ui_draw_menu());

    /*
     * Footer.
     */
    ESP_ERROR_CHECK(
        display_fill_rect(
            0,
            204,
            DISPLAY_WIDTH,
            1,
            DISPLAY_COLOR_DARK_GRAY
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            10,
            218,
            "UP/DOWN SELECT",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            244,
            218,
            "OK OPEN",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    return ESP_OK;
}


static esp_err_t ui_draw_detail(void)
{
    ESP_ERROR_CHECK(
        display_fill(DISPLAY_COLOR_BLACK)
    );

    ESP_ERROR_CHECK(ui_draw_header());

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            54,
            MENU_ITEMS[s_selected_item],
            DISPLAY_COLOR_CYAN,
            DISPLAY_COLOR_BLACK,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            100,
            "PLACEHOLDER SCREEN",
            DISPLAY_COLOR_WHITE,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            12,
            220,
            "LEFT BACK",
            DISPLAY_COLOR_GRAY,
            DISPLAY_COLOR_BLACK,
            1
        )
    );

    return ESP_OK;
}


esp_err_t ui_init(void)
{
    s_screen = UI_SCREEN_HOME;
    s_selected_item = 0;

    ESP_LOGI(TAG, "Initializing UI");

    esp_err_t err = ui_draw_home();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UI ready");
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
                        MENU_ITEM_COUNT - 1;
                }
                else {
                    s_selected_item--;
                }

                ESP_ERROR_CHECK(ui_draw_menu());
                break;


            case KEYPAD_KEY_DOWN:

                s_selected_item++;

                if (s_selected_item >= MENU_ITEM_COUNT) {
                    s_selected_item = 0;
                }

                ESP_ERROR_CHECK(ui_draw_menu());
                break;


            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_RIGHT:

                s_screen = UI_SCREEN_DETAIL;

                ESP_ERROR_CHECK(ui_draw_detail());
                break;


            case KEYPAD_KEY_LEFT:
            case KEYPAD_KEY_NONE:
            default:
                break;
        }

        return;
    }


    if (s_screen == UI_SCREEN_DETAIL) {

        switch (key) {
            case KEYPAD_KEY_LEFT:

                s_screen = UI_SCREEN_HOME;

                ESP_ERROR_CHECK(ui_draw_home());
                break;


            case KEYPAD_KEY_UP:
            case KEYPAD_KEY_DOWN:
            case KEYPAD_KEY_RIGHT:
            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_NONE:
            default:
                break;
        }
    }
}