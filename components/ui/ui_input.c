#include "ui.h"

#include "esp_log.h"

#include "ui_navigation.h"
#include "ui_screen.h"
#include "ui_screen_router.h"
#include "ui_state.h"


static const char *TAG =
    "UI";


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

    ui_screen_t screen =
        ui_state_screen();

    esp_err_t err =
        ui_screen_handle_key(
            screen,
            key
        );

    if (err == ESP_OK) {
        return;
    }

    if (
        err ==
            ESP_ERR_NOT_SUPPORTED &&
        screen !=
            UI_SCREEN_HOME &&
        key ==
            KEYPAD_KEY_LEFT
    ) {
        err =
            ui_navigation_return_home();

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to return home: %s",
                esp_err_to_name(err)
            );
        }

        return;
    }

    ESP_LOGE(
        TAG,
        "%s navigation failed: %s",
        ui_screen_name(screen),
        esp_err_to_name(err)
    );
}
