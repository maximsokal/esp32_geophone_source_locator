#include "ui_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_log.h"

#include "storage_service.h"
#include "ui_storage_view.h"


static const char *TAG =
    "UI_STORAGE";


typedef enum
{
    STORAGE_MODE_MENU = 0,
    STORAGE_MODE_CARD_INFO,
    STORAGE_MODE_FREE_SPACE,
    STORAGE_MODE_ERASE_CONFIRM,
    STORAGE_MODE_ERASING

} storage_mode_t;


static storage_mode_t s_mode =
    STORAGE_MODE_MENU;


static storage_mode_t s_requested_mode =
    STORAGE_MODE_CARD_INFO;


static size_t s_selected_action =
    0;


static char s_notice[40] =
    "";


static bool s_notice_warning =
    false;


static sd_card_info_t s_info;


static esp_err_t draw_menu(void)
{
    return ui_storage_view_draw_menu(
        s_selected_action,
        s_notice,
        s_notice_warning
    );
}


static void storage_service_event(
    const storage_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    if (event == NULL) {
        return;
    }

    esp_err_t draw_err =
        ESP_OK;

    switch (event->type) {
        case STORAGE_SERVICE_EVENT_INFO_DONE:
            s_info =
                event->info;

            s_mode =
                s_requested_mode;

            draw_err =
                s_mode ==
                    STORAGE_MODE_CARD_INFO
                ? ui_storage_view_draw_card_info(
                    &s_info
                )
                : ui_storage_view_draw_free_space(
                    &s_info
                );
            break;

        case STORAGE_SERVICE_EVENT_INFO_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "CARD INFO FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                STORAGE_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case STORAGE_SERVICE_EVENT_ERASE_DONE:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "DATA ERASED"
            );

            s_notice_warning =
                false;

            s_mode =
                STORAGE_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case STORAGE_SERVICE_EVENT_ERASE_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "ERASE FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                STORAGE_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        default:
            break;
    }

    if (draw_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Background redraw failed: %s",
            esp_err_to_name(
                draw_err
            )
        );
    }
}


static esp_err_t request_info(
    storage_mode_t mode
)
{
    s_requested_mode =
        mode;

    ESP_ERROR_CHECK(
        ui_common_draw_footer(
            "LOADING CARD INFO..."
        )
    );

    return storage_service_info_async();
}


static esp_err_t update_menu_selection(
    keypad_key_t key
)
{
    size_t previous =
        s_selected_action;

    if (
        key ==
        KEYPAD_KEY_DOWN
    ) {
        s_selected_action =
            (
                s_selected_action +
                1
            ) %
            UI_STORAGE_ACTION_COUNT;
    }
    else {
        s_selected_action =
            s_selected_action == 0
                ? UI_STORAGE_ACTION_COUNT - 1
                : s_selected_action - 1;
    }

    return ui_storage_view_update_selection(
        previous,
        s_selected_action
    );
}


static esp_err_t handle_menu_key(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
            return update_menu_selection(
                key
            );

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            switch (s_selected_action) {
                case 0:
                    return request_info(
                        STORAGE_MODE_CARD_INFO
                    );

                case 1:
                    return request_info(
                        STORAGE_MODE_FREE_SPACE
                    );

                case 2:
                    s_mode =
                        STORAGE_MODE_ERASE_CONFIRM;

                    return
                        ui_storage_view_draw_erase_confirm();

                default:
                    return ESP_ERR_INVALID_STATE;
            }

        case KEYPAD_KEY_LEFT:
            return ESP_ERR_NOT_SUPPORTED;

        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


esp_err_t ui_storage_open(void)
{
    ESP_ERROR_CHECK(
        storage_service_set_event_callback(
            storage_service_event,
            NULL
        )
    );

    s_mode =
        STORAGE_MODE_MENU;

    s_selected_action =
        0;

    s_notice[0] =
        '\0';

    return draw_menu();
}


esp_err_t ui_storage_draw(void)
{
    switch (s_mode) {
        case STORAGE_MODE_CARD_INFO:
            return
                ui_storage_view_draw_card_info(
                    &s_info
                );

        case STORAGE_MODE_FREE_SPACE:
            return
                ui_storage_view_draw_free_space(
                    &s_info
                );

        case STORAGE_MODE_ERASE_CONFIRM:
            return
                ui_storage_view_draw_erase_confirm();

        case STORAGE_MODE_ERASING:
            return
                ui_storage_view_draw_erasing();

        case STORAGE_MODE_MENU:
        default:
            return draw_menu();
    }
}


esp_err_t ui_storage_handle_key(
    keypad_key_t key
)
{
    if (
        storage_service_is_busy()
    ) {
        return ESP_OK;
    }

    if (
        s_mode ==
        STORAGE_MODE_MENU
    ) {
        return handle_menu_key(
            key
        );
    }

    if (
        s_mode ==
        STORAGE_MODE_ERASE_CONFIRM
    ) {
        switch (key) {
            case KEYPAD_KEY_LEFT:
                s_mode =
                    STORAGE_MODE_MENU;

                return draw_menu();

            case KEYPAD_KEY_OK:
            case KEYPAD_KEY_RIGHT:
            {
                /*
                 * Draw the complete busy screen before
                 * starting the asynchronous erase worker.
                 */
                s_mode =
                    STORAGE_MODE_ERASING;

                ESP_ERROR_CHECK(
                    ui_storage_view_draw_erasing()
                );

                esp_err_t err =
                    storage_service_erase_data_async();

                if (err != ESP_OK) {
                    s_mode =
                        STORAGE_MODE_ERASE_CONFIRM;

                    return err;
                }

                return ESP_OK;
            }

            case KEYPAD_KEY_UP:
            case KEYPAD_KEY_DOWN:
            case KEYPAD_KEY_NONE:
            default:
                return ESP_OK;
        }
    }

    if (
        key ==
        KEYPAD_KEY_LEFT
    ) {
        s_mode =
            STORAGE_MODE_MENU;

        return draw_menu();
    }

    return ESP_OK;
}