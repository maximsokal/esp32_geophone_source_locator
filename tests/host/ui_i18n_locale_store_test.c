#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ui_i18n_locale_store.h"

typedef struct {
    bool read_ok;
    bool write_ok;
    bool has_value;
    uint8_t value;
    unsigned reads;
    unsigned writes;
    char name_space[8];
    char key[8];
} fake_storage_t;

static bool fake_read_u8(void *context, const char *name_space, const char *key, uint8_t *value)
{
    fake_storage_t *storage = context;
    ++storage->reads;
    assert(strcmp(name_space, "ui") == 0);
    assert(strcmp(key, "lang") == 0);
    if (!storage->read_ok || !storage->has_value) {
        return false;
    }
    *value = storage->value;
    return true;
}

static bool fake_write_u8(void *context, const char *name_space, const char *key, uint8_t value)
{
    fake_storage_t *storage = context;
    ++storage->writes;
    assert(strcmp(name_space, "ui") == 0);
    assert(strcmp(key, "lang") == 0);
    strcpy(storage->name_space, name_space);
    strcpy(storage->key, key);
    if (!storage->write_ok) {
        return false;
    }
    storage->value = value;
    storage->has_value = true;
    return true;
}

int main(void)
{
    const ui_i18n_locale_storage_t ops = {
        .read_u8 = fake_read_u8,
        .write_u8 = fake_write_u8,
    };
    fake_storage_t storage = {.read_ok = true, .write_ok = true};

    assert(ui_i18n_locale_storage_load(&ops, &storage) == UI_LOCALE_EN);
    storage.has_value = true;
    storage.value = 0U;
    assert(ui_i18n_locale_storage_load(&ops, &storage) == UI_LOCALE_EN);
    storage.value = 1U;
    assert(ui_i18n_locale_storage_load(&ops, &storage) == UI_LOCALE_UK);
    storage.value = 2U;
    assert(ui_i18n_locale_storage_load(&ops, &storage) == UI_LOCALE_EN);
    storage.read_ok = false;
    assert(ui_i18n_locale_storage_load(&ops, &storage) == UI_LOCALE_EN);

    storage.read_ok = true;
    assert(ui_i18n_locale_storage_save(&ops, &storage, UI_LOCALE_EN));
    assert(storage.value == 0U);
    assert(strcmp(storage.name_space, "ui") == 0);
    assert(strcmp(storage.key, "lang") == 0);
    assert(ui_i18n_locale_storage_save(&ops, &storage, UI_LOCALE_UK));
    assert(storage.value == 1U);

    const unsigned writes_before_invalid = storage.writes;
    assert(!ui_i18n_locale_storage_save(&ops, &storage, (ui_locale_t)99));
    assert(storage.writes == writes_before_invalid);

    storage.write_ok = false;
    const uint8_t persisted_before_failure = storage.value;
    assert(!ui_i18n_locale_storage_save(&ops, &storage, UI_LOCALE_EN));
    assert(storage.value == persisted_before_failure);

    storage.write_ok = true;
    for (unsigned i = 0U; i < 1000U; ++i) {
        const ui_locale_t locale = (i & 1U) != 0U ? UI_LOCALE_UK : UI_LOCALE_EN;
        assert(ui_i18n_locale_storage_save(&ops, &storage, locale));
        assert(ui_i18n_locale_storage_load(&ops, &storage) == locale);
    }

    return 0;
}
