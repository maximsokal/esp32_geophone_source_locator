/* generated — do not edit */
#ifndef UI_I18N_CATALOG_GEN_H
#define UI_I18N_CATALOG_GEN_H

typedef enum {
    UI_I18N_KEY_APP_TITLE = 0,
    UI_I18N_KEY_MENU_MEASUREMENTS = 1,
    UI_I18N_KEY_MENU_OBJECTS = 2,
    UI_I18N_KEY_MENU_STORAGE = 3,
    UI_I18N_KEY_POWER_LOW_BATTERY_SOON = 4,
    UI_I18N_KEY_SYSTEM_LANGUAGE_ENGLISH = 5,
    UI_I18N_KEY_SYSTEM_LANGUAGE_FOOTER = 6,
    UI_I18N_KEY_SYSTEM_LANGUAGE_UKRAINIAN = 7,
    UI_I18N_KEY_SYSTEM_TITLE = 8,
    UI_I18N_KEY_COUNT
} ui_i18n_key_t;

const char *ui_i18n_catalog_en(ui_i18n_key_t key);
const char *ui_i18n_catalog_uk(ui_i18n_key_t key);

#endif
