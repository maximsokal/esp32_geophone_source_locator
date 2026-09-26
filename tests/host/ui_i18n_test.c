#include <assert.h>
#include <string.h>

#include "ui_i18n.h"

int main(void)
{
    ui_i18n_set_locale(UI_LOCALE_EN);
    assert(ui_i18n_get_locale() == UI_LOCALE_EN);
    for (unsigned key = 0; key < (unsigned)UI_I18N_KEY_COUNT; ++key) {
        const char *text = ui_tr((ui_i18n_key_t)key);
        assert(text != 0);
        assert(text[0] != '\0');
    }

    const char *english_title = ui_tr(UI_I18N_KEY_APP_TITLE);
    ui_i18n_set_locale(UI_LOCALE_UK);
    assert(ui_i18n_get_locale() == UI_LOCALE_UK);
    for (unsigned key = 0; key < (unsigned)UI_I18N_KEY_COUNT; ++key) {
        const char *text = ui_tr((ui_i18n_key_t)key);
        assert(text != 0);
        assert(text[0] != '\0');
    }
    assert(strcmp(ui_tr(UI_I18N_KEY_APP_TITLE), english_title) == 0);

    ui_i18n_set_locale((ui_locale_t)99);
    assert(ui_i18n_get_locale() == UI_LOCALE_EN);
    assert(strcmp(ui_tr(UI_I18N_KEY_APP_TITLE), english_title) == 0);
    assert(strcmp(ui_tr((ui_i18n_key_t)UI_I18N_KEY_COUNT), "?") == 0);
    assert(strcmp(ui_tr((ui_i18n_key_t)-1), "?") == 0);
    return 0;
}
