#pragma once

#include <stddef.h>
#include <stdint.h>

#define LOCALIZATION_MATERIAL_ID_SIZE 24U

typedef struct {
    const char *id;
    uint32_t propagation_speed_mps;
    uint32_t propagation_speed_uncertainty_mps;
    float v_min_mps;
    float v_max_mps;
    float vp_vs_min;
    float vp_vs_max;
} localization_material_preset_t;

const localization_material_preset_t *localization_material_preset_lookup(const char *id);
size_t localization_material_preset_count(void);
const localization_material_preset_t *localization_material_preset_at(size_t index);
