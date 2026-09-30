#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_i18n.h"
#include "ui_i18n_locale_store.h"

static bool valid_utf8_string(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;
    const size_t length = strlen(text);
    size_t offset = 0U;

    assert(bytes[length] == '\0');
    while (offset < length) {
        const unsigned char first = bytes[offset];
        uint32_t codepoint;
        uint32_t minimum;
        size_t trailing;
        if (first <= 0x7FU) {
            codepoint = first;
            minimum = 0U;
            trailing = 0U;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            codepoint = (uint32_t)(first & 0x1FU);
            minimum = 0x80U;
            trailing = 1U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            codepoint = (uint32_t)(first & 0x0FU);
            minimum = 0x800U;
            trailing = 2U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            codepoint = (uint32_t)(first & 0x07U);
            minimum = 0x10000U;
            trailing = 3U;
        } else {
            return false;
        }

        if (trailing > length - offset - 1U) {
            return false;
        }
        for (size_t index = 1U; index <= trailing; ++index) {
            const unsigned char continuation = bytes[offset + index];
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (uint32_t)(continuation & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        offset += trailing + 1U;
    }
    return true;
}

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

static void test_catalog_lookup_utf8_and_unknown_ids(void)
{
    for (unsigned key = 0U; key < (unsigned)UI_I18N_KEY_COUNT; ++key) {
        for (unsigned locale = 0U; locale < 2U; ++locale) {
            ui_i18n_set_locale(locale == 0U ? UI_LOCALE_EN : UI_LOCALE_UK);
            const char *text = ui_tr((ui_i18n_key_t)key);
            assert(text != NULL);
            assert(valid_utf8_string(text));
        }
    }

    uint32_t random = 0x189C0FFEU;
    for (unsigned sample = 0U; sample < 512U; ++sample) {
        const ui_locale_t unknown_locale = (ui_locale_t)(2U + next_random(&random) % 4096U);
        ui_i18n_set_locale(unknown_locale);
        assert(ui_i18n_get_locale() == UI_LOCALE_EN);

        int raw_key = (int)UI_I18N_KEY_COUNT + 1 + (int)(next_random(&random) % 4096U);
        if ((next_random(&random) & 1U) != 0U) {
            raw_key = -raw_key;
        }
        const char *text = ui_tr((ui_i18n_key_t)raw_key);
        assert(text != NULL);
        assert(strcmp(text, "?") == 0);
    }
}

typedef struct {
    bool has_locale;
    bool read_ok;
    uint8_t locale;
    unsigned reads;
    unsigned writes;
    uint32_t unrelated_setting;
} fake_storage_t;

static bool read_locale(void *context, const char *name_space, const char *key, uint8_t *value)
{
    fake_storage_t *storage = context;
    ++storage->reads;
    assert(strcmp(name_space, "ui") == 0);
    assert(strcmp(key, "lang") == 0);
    if (!storage->read_ok || !storage->has_locale) {
        return false;
    }
    *value = storage->locale;
    return true;
}

static bool write_locale(void *context, const char *name_space, const char *key, uint8_t value)
{
    fake_storage_t *storage = context;
    ++storage->writes;
    assert(strcmp(name_space, "ui") == 0);
    assert(strcmp(key, "lang") == 0);
    storage->locale = value;
    storage->has_locale = true;
    return true;
}

static void test_persisted_locale_falls_back_without_touching_other_settings(void)
{
    const ui_i18n_locale_storage_t operations = {
        .read_u8 = read_locale,
        .write_u8 = write_locale,
    };
    fake_storage_t storage = {
        .read_ok = true,
        .unrelated_setting = 0x189U,
    };

    assert(ui_i18n_locale_storage_load(&operations, &storage) == UI_LOCALE_EN);
    storage.has_locale = true;
    storage.locale = 2U;
    assert(ui_i18n_locale_storage_load(&operations, &storage) == UI_LOCALE_EN);
    storage.locale = UINT8_MAX;
    assert(ui_i18n_locale_storage_load(&operations, &storage) == UI_LOCALE_EN);
    storage.locale = (uint8_t)UI_LOCALE_UK;
    assert(ui_i18n_locale_storage_load(&operations, &storage) == UI_LOCALE_UK);
    storage.read_ok = false;
    assert(ui_i18n_locale_storage_load(&operations, &storage) == UI_LOCALE_EN);

    assert(storage.writes == 0U);
    assert(storage.unrelated_setting == 0x189U);
}

int main(void)
{
    test_catalog_lookup_utf8_and_unknown_ids();
    test_persisted_locale_falls_back_without_touching_other_settings();
    return 0;
}
