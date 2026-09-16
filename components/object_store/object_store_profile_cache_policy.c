#include "object_store_internal.h"


void object_store_profile_cache_invalidate_membership_locked(void)
{
    float defaults[OBJECT_STORE_POD_COUNT];
    const esp_err_t settings_err =
        object_store_settings_load_locked(defaults);

    /* Profile membership is derived from the bounded archive census. Reset it
     * after topology/ACTIVE changes, but restore the independent settings
     * cache so those mutations do not force an unrelated settings.json read. */
    object_store_profile_cache_reset();

    if (settings_err != ESP_OK) {
        return;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        object_store_profile_cache_note_default_locked(i, defaults[i]);
    }
}
