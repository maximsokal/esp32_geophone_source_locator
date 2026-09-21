#include "localization_comparison.h"

#include <math.h>
#include <string.h>


static double distance_m(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


esp_err_t localization_comparison_fuse(
    const localization_comparison_method_result_t *methods,
    size_t method_count,
    localization_comparison_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (localization_comparison_result_t) { 0 };

    if (methods == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (method_count == 0u || method_count > LOCALIZATION_COMPARISON_MAX_METHODS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0u; i < method_count; ++i) {
        if (methods[i].method_name[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }

        if (methods[i].solved) {
            if (
                !isfinite(methods[i].position.x_m) ||
                !isfinite(methods[i].position.y_m) ||
                !isfinite(methods[i].position.z_m)
            ) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (methods[i].rejection_reason[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memcpy(result->methods, methods, method_count * sizeof(methods[0]));
    result->method_count = method_count;

    double sum_weight = 0.0;
    double sum_weighted_x = 0.0;
    double sum_weighted_y = 0.0;
    double sum_weighted_z = 0.0;

    for (size_t i = 0u; i < method_count; ++i) {
        if (!methods[i].solved || methods[i].quality <= 0.0) {
            continue;
        }

        const double weight = methods[i].quality;
        sum_weight += weight;
        sum_weighted_x += weight * methods[i].position.x_m;
        sum_weighted_y += weight * methods[i].position.y_m;
        sum_weighted_z += weight * methods[i].position.z_m;
        ++result->joint_methods_used_count;
    }

    if (sum_weight > 0.0) {
        result->joint_found = true;
        result->joint_position = (localization_point3d_t) {
            .x_m = sum_weighted_x / sum_weight,
            .y_m = sum_weighted_y / sum_weight,
            .z_m = sum_weighted_z / sum_weight
        };
        result->joint_total_quality = sum_weight;
    }

    double max_disagreement_m = 0.0;
    for (size_t i = 0u; i < method_count; ++i) {
        if (!methods[i].solved) {
            continue;
        }
        for (size_t j = i + 1u; j < method_count; ++j) {
            if (!methods[j].solved) {
                continue;
            }
            const double disagreement = distance_m(methods[i].position, methods[j].position);
            if (disagreement > max_disagreement_m) {
                max_disagreement_m = disagreement;
            }
        }
    }
    result->max_pairwise_disagreement_m = max_disagreement_m;

    return ESP_OK;
}
