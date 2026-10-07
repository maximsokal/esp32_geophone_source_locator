#pragma once

#include "hw_inventory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return a stable snake_case code for a firmware diagnostic reason.
 * NULL and empty reasons map to "none"; unknown text maps to "unrecognized".
 * The module id is reserved for module-specific mappings. */
const char *hw_inventory_reason_code(hw_module_id_t id, const char *reason);

#ifdef __cplusplus
}
#endif
