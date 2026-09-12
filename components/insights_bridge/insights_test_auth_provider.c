#include "esp_err.h"


/* This is intentionally a public, non-secret fixture for the enabled build
 * profile. Production credentials remain outside the repository. */
const char *app_esp_insights_auth_key(void)
{
    return "geolocator-test-auth-key";
}

