#pragma once

#include <stdbool.h>

#include "ui_i18n.h"

typedef enum {
    UI_SYSTEM_ENTRY_ENGLISH = 0,
    UI_SYSTEM_ENTRY_UKRAINIAN = 1,
    UI_SYSTEM_ENTRY_HARDWARE = 2,
    UI_SYSTEM_ENTRY_COUNT = 3,
} ui_system_entry_t;

typedef enum {
    UI_SYSTEM_KEY_UP = 0,
    UI_SYSTEM_KEY_DOWN,
    UI_SYSTEM_KEY_LEFT,
    UI_SYSTEM_KEY_RIGHT,
    UI_SYSTEM_KEY_OK,
    UI_SYSTEM_KEY_OTHER,
} ui_system_key_t;

typedef enum {
    UI_SYSTEM_ACTION_NONE = 0,
    UI_SYSTEM_ACTION_SELECT_EN,
    UI_SYSTEM_ACTION_SELECT_UK,
    UI_SYSTEM_ACTION_SELECT_HARDWARE,
    UI_SYSTEM_ACTION_APPLY_LANGUAGE,
    UI_SYSTEM_ACTION_OPEN_HARDWARE,
    UI_SYSTEM_ACTION_CLOSE_HARDWARE,
    UI_SYSTEM_ACTION_GO_HOME,
} ui_system_action_t;

typedef struct {
    ui_system_entry_t selected;
    bool hardware_open;
} ui_system_state_t;

ui_system_entry_t ui_system_model_entry_from_locale(ui_locale_t locale);
bool ui_system_model_entry_locale(
    ui_system_entry_t entry,
    ui_locale_t *locale
);

ui_system_action_t ui_system_model_on_key(
    const ui_system_state_t *state,
    ui_system_key_t key,
    bool selected_language_is_active,
    ui_system_state_t *next
);
