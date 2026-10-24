#include "object_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "json_number.h"


static int32_t object_store_geometry_json_non_negative_mm(const cJSON *item)
{
    int32_t value = 0;
    return
        cJSON_IsNumber(item) &&
        json_number_to_int32(item->valuedouble, &value) &&
        value >= 0
        ? value
        : 0;
}


void object_store_geometry_from_json(
    const cJSON *root,
    object_store_geometry_t *geometry
)
{
    if (root == NULL || geometry == NULL) {
        return;
    }

    /* geometry defaults to zero/not-computed via the caller's memset; absent
     * or malformed JSON leaves it exactly there -- old objects written before
     * MAX-118 have no "geometry" key at all. */
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    if (!cJSON_IsObject(node)) {
        return;
    }

    const cJSON *anchored = cJSON_GetObjectItemCaseSensitive(node, "anchored");
    geometry->anchored = cJSON_IsTrue(anchored);

    const cJSON *pods = cJSON_GetObjectItemCaseSensitive(node, "pods");
    if (!cJSON_IsArray(pods)) {
        return;
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        const cJSON *pod = cJSON_GetArrayItem(pods, (int)i);
        if (!cJSON_IsObject(pod)) {
            continue;
        }
        const cJSON *computed = cJSON_GetObjectItemCaseSensitive(pod, "computed");
        if (!cJSON_IsTrue(computed)) {
            continue;
        }
        const cJSON *x_mm = cJSON_GetObjectItemCaseSensitive(pod, "x_mm");
        const cJSON *y_mm = cJSON_GetObjectItemCaseSensitive(pod, "y_mm");
        const cJSON *z_mm = cJSON_GetObjectItemCaseSensitive(pod, "z_mm");

        /* Validate every coordinate before touching the pod: a value that is
         * not a finite integer inside int32 must not become a position (the
         * bare cast would be undefined behaviour) nor mark the pod computed. */
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        if (
            !cJSON_IsNumber(x_mm) || !json_number_to_int32(x_mm->valuedouble, &x) ||
            !cJSON_IsNumber(y_mm) || !json_number_to_int32(y_mm->valuedouble, &y) ||
            !cJSON_IsNumber(z_mm) || !json_number_to_int32(z_mm->valuedouble, &z)
        ) {
            continue;
        }

        const cJSON *computed_at = cJSON_GetObjectItemCaseSensitive(pod, "computed_at_ms");
        uint64_t computed_at_ms = 0U;
        if (
            !cJSON_IsNumber(computed_at) ||
            !json_number_to_uint64(computed_at->valuedouble, &computed_at_ms)
        ) {
            computed_at_ms = 0U;
        }

        object_store_pod_geometry_t *target = &geometry->pods[i];
        target->leveled = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(pod, "leveled")
        );
        target->x_mm = x;
        target->y_mm = y;
        target->z_mm = z;
        target->residual_mm = object_store_geometry_json_non_negative_mm(
            cJSON_GetObjectItemCaseSensitive(pod, "residual_mm")
        );
        target->uncertainty_mm = object_store_geometry_json_non_negative_mm(
            cJSON_GetObjectItemCaseSensitive(pod, "uncertainty_mm")
        );
        target->computed_at_ms = computed_at_ms;
        target->computed = true;
    }
}
