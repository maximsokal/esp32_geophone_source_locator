#include "object_store_internal.h"


bool object_store_profile_cache_create_requires_membership_refresh(
    bool cache_valid,
    size_t cache_count,
    bool object_present
)
{
    return
        cache_valid &&
        !object_present &&
        cache_count >= OBJECT_STORE_MAX_OBJECTS;
}


bool object_store_profile_cache_active_requires_membership_refresh(
    bool cache_valid,
    bool target_present,
    const char *object_id
)
{
    return
        cache_valid &&
        object_id != NULL &&
        object_id[0] != '\0' &&
        !target_present;
}


bool object_store_profile_cache_delete_requires_membership_refresh(
    bool cache_valid,
    size_t cache_count,
    bool object_present
)
{
    return
        cache_valid &&
        object_present &&
        cache_count >= OBJECT_STORE_MAX_OBJECTS;
}
