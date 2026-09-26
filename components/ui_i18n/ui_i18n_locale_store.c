#include "ui_i18n_locale_store.h"

#include <stddef.h>

ui_locale_t ui_i18n_locale_storage_load(const ui_i18n_locale_storage_t *storage, void *context)
{
    if (storage == NULL || storage->read_u8 == NULL) {
        return UI_LOCALE_EN;
    }

    uint8_t value = UINT8_MAX;
    if (!storage->read_u8(context, "ui", "lang", &value)) {
        return UI_LOCALE_EN;
    }

    return value == (uint8_t)UI_LOCALE_UK ? UI_LOCALE_UK : UI_LOCALE_EN;
}

bool ui_i18n_locale_storage_save(const ui_i18n_locale_storage_t *storage, void *context, ui_locale_t locale)
{
    if (
        storage == NULL ||
        storage->write_u8 == NULL ||
        (locale != UI_LOCALE_EN && locale != UI_LOCALE_UK)
    ) {
        return false;
    }

    return storage->write_u8(context, "ui", "lang", (uint8_t)locale);
}
