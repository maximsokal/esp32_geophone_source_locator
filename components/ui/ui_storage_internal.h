#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "keypad.h"
#include "sd_card.h"
#include "storage_service.h"


typedef enum
{
    STORAGE_MODE_MENU = 0,
    STORAGE_MODE_CARD_INFO,
    STORAGE_MODE_FREE_SPACE,
    STORAGE_MODE_ERASE_CONFIRM,
    STORAGE_MODE_ERASING

} storage_mode_t;


typedef struct
{
    storage_mode_t mode;

    storage_mode_t requested_mode;

    size_t selected_action;

    char notice[40];

    bool notice_warning;

    sd_card_info_t info;

} ui_storage_state_t;


ui_storage_state_t *ui_storage_state(void);


void ui_storage_state_reset(void);


esp_err_t ui_storage_draw_menu_internal(void);


esp_err_t ui_storage_draw_current_internal(void);


void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
);


esp_err_t ui_storage_request_info(
    storage_mode_t mode
);


esp_err_t ui_storage_start_erase(void);


esp_err_t ui_storage_handle_menu_key(
    keypad_key_t key
);


esp_err_t ui_storage_handle_input(
    keypad_key_t key
);
