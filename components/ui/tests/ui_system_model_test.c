#include <assert.h>
#include <stddef.h>

#include "ui_system_model.h"


static ui_system_state_t make_state(
    ui_system_entry_t selected,
    bool hardware_open
)
{
    return (ui_system_state_t){
        .selected = selected,
        .hardware_open = hardware_open,
    };
}


static void expect_state(
    ui_system_state_t actual,
    ui_system_state_t expected
)
{
    assert(actual.selected == expected.selected);
    assert(actual.hardware_open == expected.hardware_open);
}


static void expect_key(
    ui_system_state_t state,
    ui_system_key_t key,
    bool selected_language_is_active,
    ui_system_action_t expected_action,
    ui_system_state_t expected_state
)
{
    ui_system_state_t next;
    const ui_system_action_t action = ui_system_model_on_key(
        &state,
        key,
        selected_language_is_active,
        &next
    );

    assert(action == expected_action);
    expect_state(next, expected_state);
}


static void test_entry_locale_mapping(void)
{
    ui_locale_t locale = UI_LOCALE_EN;
    assert(ui_system_model_entry_from_locale(UI_LOCALE_EN) == UI_SYSTEM_ENTRY_ENGLISH);
    assert(ui_system_model_entry_from_locale(UI_LOCALE_UK) == UI_SYSTEM_ENTRY_UKRAINIAN);

    assert(ui_system_model_entry_locale(UI_SYSTEM_ENTRY_ENGLISH, &locale));
    assert(locale == UI_LOCALE_EN);
    assert(ui_system_model_entry_locale(UI_SYSTEM_ENTRY_UKRAINIAN, &locale));
    assert(locale == UI_LOCALE_UK);
    assert(!ui_system_model_entry_locale(UI_SYSTEM_ENTRY_HARDWARE, &locale));
    assert(!ui_system_model_entry_locale(UI_SYSTEM_ENTRY_ENGLISH, NULL));
}


static void test_system_key_matrix(void)
{
    static const ui_system_key_t keys[] = {
        UI_SYSTEM_KEY_UP,
        UI_SYSTEM_KEY_DOWN,
        UI_SYSTEM_KEY_LEFT,
        UI_SYSTEM_KEY_RIGHT,
        UI_SYSTEM_KEY_OK,
        UI_SYSTEM_KEY_OTHER,
    };
    static const ui_system_action_t expected_actions[UI_SYSTEM_ENTRY_COUNT][6] = {
        {
            UI_SYSTEM_ACTION_NONE,
            UI_SYSTEM_ACTION_SELECT_UK,
            UI_SYSTEM_ACTION_GO_HOME,
            UI_SYSTEM_ACTION_APPLY_LANGUAGE,
            UI_SYSTEM_ACTION_APPLY_LANGUAGE,
            UI_SYSTEM_ACTION_NONE,
        },
        {
            UI_SYSTEM_ACTION_SELECT_EN,
            UI_SYSTEM_ACTION_SELECT_HARDWARE,
            UI_SYSTEM_ACTION_GO_HOME,
            UI_SYSTEM_ACTION_APPLY_LANGUAGE,
            UI_SYSTEM_ACTION_APPLY_LANGUAGE,
            UI_SYSTEM_ACTION_NONE,
        },
        {
            UI_SYSTEM_ACTION_SELECT_UK,
            UI_SYSTEM_ACTION_NONE,
            UI_SYSTEM_ACTION_GO_HOME,
            UI_SYSTEM_ACTION_OPEN_HARDWARE,
            UI_SYSTEM_ACTION_OPEN_HARDWARE,
            UI_SYSTEM_ACTION_NONE,
        },
    };

    for (ui_system_entry_t entry = UI_SYSTEM_ENTRY_ENGLISH;
         entry < UI_SYSTEM_ENTRY_COUNT;
         entry = (ui_system_entry_t)(entry + 1)) {
        for (size_t key_index = 0U;
             key_index < sizeof(keys) / sizeof(keys[0]);
             ++key_index) {
            const ui_system_state_t state = make_state(entry, false);
            ui_system_state_t expected_state = state;
            const ui_system_action_t expected_action =
                expected_actions[entry][key_index];

            if (expected_action == UI_SYSTEM_ACTION_SELECT_EN) {
                expected_state.selected = UI_SYSTEM_ENTRY_ENGLISH;
            } else if (expected_action == UI_SYSTEM_ACTION_SELECT_UK) {
                expected_state.selected = UI_SYSTEM_ENTRY_UKRAINIAN;
            } else if (expected_action == UI_SYSTEM_ACTION_SELECT_HARDWARE) {
                expected_state.selected = UI_SYSTEM_ENTRY_HARDWARE;
            } else if (expected_action == UI_SYSTEM_ACTION_OPEN_HARDWARE) {
                expected_state.hardware_open = true;
            }

            expect_key(
                state,
                keys[key_index],
                false,
                expected_action,
                expected_state
            );
        }
    }
}


static void test_active_language_apply_is_noop(void)
{
    static const ui_system_entry_t language_entries[] = {
        UI_SYSTEM_ENTRY_ENGLISH,
        UI_SYSTEM_ENTRY_UKRAINIAN,
    };
    static const ui_system_key_t apply_keys[] = {
        UI_SYSTEM_KEY_RIGHT,
        UI_SYSTEM_KEY_OK,
    };

    for (size_t entry_index = 0U;
         entry_index < sizeof(language_entries) / sizeof(language_entries[0]);
         ++entry_index) {
        for (size_t key_index = 0U;
             key_index < sizeof(apply_keys) / sizeof(apply_keys[0]);
             ++key_index) {
            const ui_system_state_t state = make_state(
                language_entries[entry_index],
                false
            );
            expect_key(
                state,
                apply_keys[key_index],
                true,
                UI_SYSTEM_ACTION_NONE,
                state
            );
        }
    }
}


static void test_hardware_open_key_matrix(void)
{
    static const ui_system_key_t keys[] = {
        UI_SYSTEM_KEY_UP,
        UI_SYSTEM_KEY_DOWN,
        UI_SYSTEM_KEY_LEFT,
        UI_SYSTEM_KEY_RIGHT,
        UI_SYSTEM_KEY_OK,
        UI_SYSTEM_KEY_OTHER,
    };

    for (ui_system_entry_t entry = UI_SYSTEM_ENTRY_ENGLISH;
         entry < UI_SYSTEM_ENTRY_COUNT;
         entry = (ui_system_entry_t)(entry + 1)) {
        for (size_t key_index = 0U;
             key_index < sizeof(keys) / sizeof(keys[0]);
             ++key_index) {
            const ui_system_state_t state = make_state(entry, true);
            ui_system_state_t expected_state = state;
            ui_system_action_t expected_action = UI_SYSTEM_ACTION_NONE;
            if (keys[key_index] == UI_SYSTEM_KEY_LEFT) {
                expected_state.hardware_open = false;
                expected_action = UI_SYSTEM_ACTION_CLOSE_HARDWARE;
            }

            expect_key(
                state,
                keys[key_index],
                false,
                expected_action,
                expected_state
            );
        }
    }
}


static void test_selection_walk_and_clamps(void)
{
    ui_system_state_t state = make_state(UI_SYSTEM_ENTRY_ENGLISH, false);
    ui_system_state_t next;

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_UP,
        false,
        &next
    ) == UI_SYSTEM_ACTION_NONE);
    expect_state(next, state);

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        false,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_UK);
    expect_state(next, make_state(UI_SYSTEM_ENTRY_UKRAINIAN, false));
    state = next;

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        false,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_HARDWARE);
    expect_state(next, make_state(UI_SYSTEM_ENTRY_HARDWARE, false));
    state = next;

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        false,
        &next
    ) == UI_SYSTEM_ACTION_NONE);
    expect_state(next, state);

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_UP,
        false,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_UK);
    expect_state(next, make_state(UI_SYSTEM_ENTRY_UKRAINIAN, false));
    state = next;

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_UP,
        false,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_EN);
    expect_state(next, make_state(UI_SYSTEM_ENTRY_ENGLISH, false));
}


static void test_language_and_hardware_regressions(void)
{
    ui_system_state_t state = make_state(UI_SYSTEM_ENTRY_ENGLISH, false);
    ui_system_state_t next;

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        true,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_UK);
    state = next;
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_RIGHT,
        false,
        &next
    ) == UI_SYSTEM_ACTION_APPLY_LANGUAGE);
    expect_state(next, state);

    state = make_state(UI_SYSTEM_ENTRY_UKRAINIAN, false);
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_UP,
        true,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_EN);
    state = next;
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_OK,
        false,
        &next
    ) == UI_SYSTEM_ACTION_APPLY_LANGUAGE);
    expect_state(next, state);

    state = make_state(UI_SYSTEM_ENTRY_ENGLISH, false);
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        true,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_UK);
    state = next;
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_DOWN,
        false,
        &next
    ) == UI_SYSTEM_ACTION_SELECT_HARDWARE);
    state = next;
    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_RIGHT,
        false,
        &next
    ) == UI_SYSTEM_ACTION_OPEN_HARDWARE);
    state = next;
    assert(state.hardware_open);
    assert(state.selected == UI_SYSTEM_ENTRY_HARDWARE);

    assert(ui_system_model_on_key(
        &state,
        UI_SYSTEM_KEY_LEFT,
        false,
        &next
    ) == UI_SYSTEM_ACTION_CLOSE_HARDWARE);
    assert(!next.hardware_open);
    assert(next.selected == UI_SYSTEM_ENTRY_HARDWARE);
}


int main(void)
{
    test_entry_locale_mapping();
    test_system_key_matrix();
    test_active_language_apply_is_noop();
    test_hardware_open_key_matrix();
    test_selection_walk_and_clamps();
    test_language_and_hardware_regressions();
    return 0;
}
