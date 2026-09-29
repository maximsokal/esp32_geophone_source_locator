#include "localization_material_presets.h"

#include <string.h>

static const localization_material_preset_t s_presets[] = {
    /* Product-owner approved values, Linear MAX-301 (2026-09-29); literature references pending. */
    { "concrete", 4000U, 500U, 3000.0f, 4800.0f, 1.55f, 1.80f },
    { "reinforced_concrete", 4200U, 500U, 3200.0f, 5000.0f, 1.55f, 1.80f },
    { "brick_masonry", 2200U, 700U, 1200.0f, 3500.0f, 1.50f, 1.90f },
    { "rubble", 400U, 250U, 150.0f, 1500.0f, 1.50f, 2.50f },
    { "steel", 5200U, 700U, 4800.0f, 6000.0f, 1.75f, 1.95f },
    { "soil_dry", 500U, 300U, 150.0f, 800.0f, 1.50f, 2.50f },
    { "soil_saturated", 1500U, 500U, 1400.0f, 2000.0f, 2.50f, 8.00f },
};

const localization_material_preset_t *localization_material_preset_lookup(const char *id)
{
    if (id == NULL) return NULL;
    for (size_t i = 0U; i < localization_material_preset_count(); ++i) {
        if (strcmp(id, s_presets[i].id) == 0) return &s_presets[i];
    }
    return NULL;
}

size_t localization_material_preset_count(void)
{
    return sizeof(s_presets) / sizeof(s_presets[0]);
}

const localization_material_preset_t *localization_material_preset_at(size_t index)
{
    return index < localization_material_preset_count() ? &s_presets[index] : NULL;
}
