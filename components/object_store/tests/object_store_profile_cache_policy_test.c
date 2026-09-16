#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "object_store_internal.h"

#include "../object_store_profile_cache_policy.c"


static void test_create_policy(void)
{
    assert(!object_store_profile_cache_create_requires_membership_refresh(false, 32U, false));
    assert(!object_store_profile_cache_create_requires_membership_refresh(true, 31U, false));
    assert(!object_store_profile_cache_create_requires_membership_refresh(true, 32U, true));
    assert(object_store_profile_cache_create_requires_membership_refresh(true, 32U, false));
}


static void test_active_policy(void)
{
    assert(!object_store_profile_cache_active_requires_membership_refresh(false, false, "OBJ_0001"));
    assert(!object_store_profile_cache_active_requires_membership_refresh(true, true, "OBJ_0001"));
    assert(!object_store_profile_cache_active_requires_membership_refresh(true, false, NULL));
    assert(!object_store_profile_cache_active_requires_membership_refresh(true, false, ""));
    assert(object_store_profile_cache_active_requires_membership_refresh(true, false, "OBJ_0001"));
}


static void test_delete_policy(void)
{
    assert(!object_store_profile_cache_delete_requires_membership_refresh(false, 32U, true));
    assert(!object_store_profile_cache_delete_requires_membership_refresh(true, 32U, false));
    assert(!object_store_profile_cache_delete_requires_membership_refresh(true, 31U, true));
    assert(object_store_profile_cache_delete_requires_membership_refresh(true, 32U, true));
}


int main(void)
{
    test_create_policy();
    test_active_policy();
    test_delete_policy();
    puts("OBJECT_STORE_PROFILE_CACHE_POLICY_HOST_PASS");
    return 0;
}
