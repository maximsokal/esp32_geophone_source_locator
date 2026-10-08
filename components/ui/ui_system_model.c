#include "ui_system_model.h"

#include <stddef.h>


ui_system_entry_t ui_system_model_entry_from_locale(ui_locale_t locale)
{
    return locale == UI_LOCALE_UK
        ? UI_SYSTEM_ENTRY_UKRAINIAN
        : UI_SYSTEM_ENTRY_ENGLISH;
}


bool ui_system_model_entry_locale(
    ui_system_entry_t entry,
    ui_locale_t *locale
)
{
    if (locale == NULL) {
        return false;
    }

    switch (entry) {
        case UI_SYSTEM_ENTRY_ENGLISH:
            *locale = UI_LOCALE_EN;
            return true;
        case UI_SYSTEM_ENTRY_UKRAINIAN:
            *locale = UI_LOCALE_UK;
            return true;
        case UI_SYSTEM_ENTRY_HARDWARE:
        case UI_SYSTEM_ENTRY_COUNT:
        default:
            return false;
    }
}


static ui_system_action_t selection_action(ui_system_entry_t entry)
{
    switch (entry) {
        case UI_SYSTEM_ENTRY_ENGLISH:
            return UI_SYSTEM_ACTION_SELECT_EN;
        case UI_SYSTEM_ENTRY_UKRAINIAN:
            return UI_SYSTEM_ACTION_SELECT_UK;
        case UI_SYSTEM_ENTRY_HARDWARE:
            return UI_SYSTEM_ACTION_SELECT_HARDWARE;
        case UI_SYSTEM_ENTRY_COUNT:
        default:
            return UI_SYSTEM_ACTION_NONE;
    }
}


static ui_system_action_t move_selection(
    ui_system_entry_t selected,
    bool down,
    ui_system_state_t *next
)
{
    if (
        selected < UI_SYSTEM_ENTRY_ENGLISH ||
        selected >= UI_SYSTEM_ENTRY_COUNT
    ) {
        return UI_SYSTEM_ACTION_NONE;
    }

    if (down) {
        if (selected + 1 >= UI_SYSTEM_ENTRY_COUNT) {
            return UI_SYSTEM_ACTION_NONE;
        }
        next->selected = (ui_system_entry_t)(selected + 1);
    } else {
        if (selected == UI_SYSTEM_ENTRY_ENGLISH) {
            return UI_SYSTEM_ACTION_NONE;
        }
        next->selected = (ui_system_entry_t)(selected - 1);
    }

    return selection_action(next->selected);
}


ui_system_action_t ui_system_model_on_key(
    const ui_system_state_t *state,
    ui_system_key_t key,
    bool selected_language_is_active,
    ui_system_state_t *next
)
{
    if (state == NULL || next == NULL) {
        return UI_SYSTEM_ACTION_NONE;
    }

    *next = *state;

    if (state->hardware_open) {
        if (key == UI_SYSTEM_KEY_LEFT) {
            next->hardware_open = false;
            return UI_SYSTEM_ACTION_CLOSE_HARDWARE;
        }
        return UI_SYSTEM_ACTION_NONE;
    }

    switch (key) {
        case UI_SYSTEM_KEY_UP:
            return move_selection(state->selected, false, next);
        case UI_SYSTEM_KEY_DOWN:
            return move_selection(state->selected, true, next);
        case UI_SYSTEM_KEY_LEFT:
            return UI_SYSTEM_ACTION_GO_HOME;
        case UI_SYSTEM_KEY_RIGHT:
        case UI_SYSTEM_KEY_OK:
            if (state->selected == UI_SYSTEM_ENTRY_HARDWARE) {
                next->hardware_open = true;
                return UI_SYSTEM_ACTION_OPEN_HARDWARE;
            }
            if (
                (state->selected == UI_SYSTEM_ENTRY_ENGLISH ||
                 state->selected == UI_SYSTEM_ENTRY_UKRAINIAN) &&
                !selected_language_is_active
            ) {
                return UI_SYSTEM_ACTION_APPLY_LANGUAGE;
            }
            return UI_SYSTEM_ACTION_NONE;
        case UI_SYSTEM_KEY_OTHER:
        default:
            return UI_SYSTEM_ACTION_NONE;
    }
}
