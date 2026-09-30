#include "localization_material_presets.h"

#include <string.h>

/*
 * Columns: id, v, sigma (m/s), v_min, v_max (m/s), vp_vs_min, vp_vs_max.
 *
 * These are conservative engineering defaults / envelopes approved by the
 * product owner (MAX-298, MAX-301), NOT literature-exact material
 * constants. A field test-knock calibration should supersede them. The
 * range, AT_BOUND and INCONSISTENT logic is this project's extension; it
 * is not taken from the TDCAS/PSAFUVS/SUEA papers.
 *
 * Informing sources (full list with DOIs/links: Linear MAX-298):
 * - concrete:  ASTM C597-22 (UPV method; saturation, cracks and voids
 *              shift the velocity); IS 13311-1 grading bands via
 *              doi:10.1016/j.heliyon.2024.e26392.
 * - reinforced concrete: Lee & Oh 2016, doi:10.1155/2016/1548215 (slab
 *              Vp ~3.3 km/s, Vp/Vs ~1.7); doi:10.3390/buildings9090202
 *              (up to ~4.5-5.0 km/s).
 * - brick:     Azam et al. 2022, doi:10.3390/buildings12081069;
 *              Jamshidi & Sousa 2024, doi:10.3390/ma17061353.
 * - rubble / dry soil / saturated soil: proxy ranges (scree, dry and wet
 *              sand) from Bourbie, Coussy & Zinszner, Acoustics of Porous
 *              Media, via Mavko's Stanford rock-physics notes. There is no
 *              direct construction-rubble standard, and the wide Vp/Vs
 *              ranges are priors pending field calibration.
 * - steel:     ASTM E494-20(2025); mild steel Vp ~5.9 km/s,
 *              Vs ~3.2 km/s (Vp/Vs ~1.84).
 */
static const localization_material_preset_t s_presets[] = {
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
