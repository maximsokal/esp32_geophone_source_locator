/* generated — do not edit */
#include "ui_i18n_catalog.gen.h"

static const char *const s_ui_i18n_en[UI_I18N_KEY_COUNT] = {
    "GeoLocator 3D", /* app.title */
    "MEASUREMENTS / AD7606", /* menu.measurements */
    "OBJECTS", /* menu.objects */
    "STORAGE / SD CARD", /* menu.storage */
    "LOW BATTERY - CONNECT USB SOON", /* power.low_battery_soon */
};

static const char *const s_ui_i18n_uk[UI_I18N_KEY_COUNT] = {
    "", /* app.title */
    "", /* menu.measurements */
    "", /* menu.objects */
    "", /* menu.storage */
    "", /* power.low_battery_soon */
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
