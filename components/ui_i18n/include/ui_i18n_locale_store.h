#ifndef UI_I18N_LOCALE_STORE_H
#define UI_I18N_LOCALE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_i18n.h"

typedef struct {
    bool (*read_u8)(void *context, const char *name_space, const char *key, uint8_t *value);
    bool (*write_u8)(void *context, const char *name_space, const char *key, uint8_t value);
} ui_i18n_locale_storage_t;

ui_locale_t ui_i18n_locale_storage_load(const ui_i18n_locale_storage_t *storage, void *context);
bool ui_i18n_locale_storage_save(const ui_i18n_locale_storage_t *storage, void *context, ui_locale_t locale);

#endif
