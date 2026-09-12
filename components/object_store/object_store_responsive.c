#include "object_store.h"


esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    /*
     * object_store_init() builds the authoritative RAM profile index before
     * the interactive UI starts. The normal listing API already has a pure-RAM
     * fast path while that index is fresh, and its periodic reconciliation is
     * topology-only for existing objects. Reuse it instead of re-reading every
     * object.json from SD during the Web-status refresh.
     *
     * The expensive part observed on hardware was session history, which has a
     * separate cooperative implementation that releases SPI2 between session
     * metadata records. Keeping profiles on the hot index avoids duplicating
     * the 5-second boot object scan immediately after it just completed.
     */
    return object_store_list_profiles_with_defaults(
        objects,
        max_objects,
        object_count,
        default_cable_lengths_m
    );
}
