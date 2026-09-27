#include "ui_i18n.h"

static ui_locale_t s_locale = UI_LOCALE_EN;

void ui_i18n_set_locale(ui_locale_t locale)
{
    s_locale = locale == UI_LOCALE_UK ? UI_LOCALE_UK : UI_LOCALE_EN;
}

ui_locale_t ui_i18n_get_locale(void)
{
    return s_locale;
}

const char *ui_tr(ui_i18n_key_t key)
{
    const char *english = ui_i18n_catalog_en(key);
    if (english == 0) {
        return "?";
    }

    if (s_locale == UI_LOCALE_UK) {
        const char *ukrainian = ui_i18n_catalog_uk(key);
        if (ukrainian != 0 && ukrainian[0] != '\0') {
            return ukrainian;
        }
    }

    return english;
}
