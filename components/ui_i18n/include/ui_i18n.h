#ifndef UI_I18N_H
#define UI_I18N_H

#include "ui_i18n_catalog.gen.h"

typedef enum {
    UI_LOCALE_EN = 0,
    UI_LOCALE_UK = 1,
} ui_locale_t;

void ui_i18n_set_locale(ui_locale_t locale);
ui_locale_t ui_i18n_get_locale(void);
const char *ui_tr(ui_i18n_key_t key);

#endif
