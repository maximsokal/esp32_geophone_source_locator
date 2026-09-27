#ifndef UI_I18N_NVS_H
#define UI_I18N_NVS_H

#include <stdbool.h>

#include "ui_i18n.h"

ui_locale_t ui_i18n_nvs_load_locale(void);
bool ui_i18n_nvs_save_locale(ui_locale_t locale);

#endif
