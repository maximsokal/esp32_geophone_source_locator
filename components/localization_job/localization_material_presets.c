#include "localization_material_presets.h"

#include <string.h>

static const localization_material_preset_t s_presets[] = {
    { "concrete", 4000U, 500U },
    { "reinforced_concrete", 4200U, 500U },
    { "brick_masonry", 2200U, 700U },
    { "rubble", 400U, 250U },
    { "steel", 5200U, 700U },
    { "soil_dry", 500U, 300U },
    { "soil_saturated", 1500U, 500U },
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
