#pragma once

#include "esp_err.h"


/*
 * Inspect persisted object directories and recover
 * interrupted metadata writes where possible.
 *
 * Directories that match OBJ_XXXX but contain no valid
 * object metadata are removed as incomplete objects.
 */
esp_err_t object_recovery_run(void);