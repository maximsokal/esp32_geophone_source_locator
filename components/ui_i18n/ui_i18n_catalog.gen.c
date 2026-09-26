/* generated — do not edit */
#include "ui_i18n_catalog.gen.h"

static const char *const s_ui_i18n_en[UI_I18N_KEY_COUNT] = {
    "GeoLocator 3D", /* app.title */
    "MEASUREMENTS / AD7606", /* menu.measurements */
    "OBJECTS", /* menu.objects */
    "STORAGE / SD CARD", /* menu.storage */
    "LOW BATTERY - CONNECT USB SOON", /* power.low_battery_soon */
    "LANGUAGE: ENGLISH", /* system.language_english */
    "UP/DN SELECT  OK APPLY  LEFT BACK", /* system.language_footer */
    "LANGUAGE: UKRAINIAN", /* system.language_ukrainian */
    "SYSTEM", /* system.title */
};

static const char *const s_ui_i18n_uk[UI_I18N_KEY_COUNT] = {
    "", /* app.title */
    "", /* menu.measurements */
    "", /* menu.objects */
    "", /* menu.storage */
    "", /* power.low_battery_soon */
    "МОВА: АНГЛІЙСЬКА", /* system.language_english */
    "UP/DN ВИБІР  OK ЗБЕРЕГТИ  LEFT НАЗАД", /* system.language_footer */
    "МОВА: УКРАЇНСЬКА", /* system.language_ukrainian */
    "СИСТЕМА", /* system.title */
};

const char *ui_i18n_catalog_en(ui_i18n_key_t key)
{
    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {
        return 0;
    }
    return s_ui_i18n_en[(unsigned)key];
}

const char *ui_i18n_catalog_uk(ui_i18n_key_t key)
{
    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {
        return 0;
    }
    return s_ui_i18n_uk[(unsigned)key];
}
