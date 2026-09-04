#include <assert.h>
#include <string.h>

#include "web_status_policy.h"

int main(void)
{
    assert(web_status_policy_evaluate(false, false, false, false, 0, false, 4) == WEB_SYSTEM_NOT_READY);
    assert(web_status_policy_evaluate(true, true, true, false, 0, false, 4) == WEB_SYSTEM_DEGRADED);
    assert(web_status_policy_evaluate(true, true, true, false, 3, false, 4) == WEB_SYSTEM_DEGRADED);
    assert(web_status_policy_evaluate(true, true, true, false, 4, false, 4) == WEB_SYSTEM_READY);
    assert(web_status_policy_evaluate(false, false, false, false, 2, false, 4) == WEB_SYSTEM_DEGRADED);
    assert(web_status_policy_evaluate(false, false, false, true, 0, false, 4) == WEB_SYSTEM_NOT_READY);
    assert(web_status_policy_evaluate(false, true, false, false, 0, false, 4) == WEB_SYSTEM_FAULT);
    assert(web_status_policy_evaluate(true, true, true, false, 4, true, 4) == WEB_SYSTEM_FAULT);

    assert(strcmp(web_status_policy_name(WEB_SYSTEM_NOT_READY), "NOT_READY") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_DEGRADED), "DEGRADED") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_READY), "READY") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_FAULT), "FAULT") == 0);

    return 0;
}
