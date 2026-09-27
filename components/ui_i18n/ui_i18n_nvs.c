#include "ui_i18n_nvs.h"

#include "nvs.h"

#include "ui_i18n_locale_store.h"

static bool ui_i18n_nvs_read_u8(void *context, const char *name_space, const char *key, uint8_t *value)
{
    (void)context;
    nvs_handle_t handle;
    if (nvs_open(name_space, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    const esp_err_t err = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool ui_i18n_nvs_write_u8(void *context, const char *name_space, const char *key, uint8_t value)
{
    (void)context;
    nvs_handle_t handle;
    if (nvs_open(name_space, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static const ui_i18n_locale_storage_t s_storage = {
    .read_u8 = ui_i18n_nvs_read_u8,
    .write_u8 = ui_i18n_nvs_write_u8,
};

ui_locale_t ui_i18n_nvs_load_locale(void)
{
    return ui_i18n_locale_storage_load(&s_storage, NULL);
}

bool ui_i18n_nvs_save_locale(ui_locale_t locale)
{
    return ui_i18n_locale_storage_save(&s_storage, NULL, locale);
}
