#include "storage_manager.h"

static bool s_storage_ready = true;

bool storage_manager_is_ready(void)
{
    return s_storage_ready;
}

void storage_manager_test_set_ready(bool ready)
{
    s_storage_ready = ready;
}
