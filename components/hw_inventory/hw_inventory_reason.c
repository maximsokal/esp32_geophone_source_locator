#include "hw_inventory_reason.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    const char *reason;
    const char *code;
    bool prefix;
} hw_inventory_reason_entry_t;

/* Keep this table in sync with HARDWARE_REASON_LEGACY_MAP in
 * webui/src/features/dashboard/hardwareInventoryModel.ts. */
static const hw_inventory_reason_entry_t s_reason_codes[] = {
    { "ready", "ready", false },
    { "no response", "no_response", false },
    { "no filesystem", "no_filesystem", false },
    { "mount failed", "mount_failed", false },
    { "read probe failed", "read_probe_failed", false },
    { "topology probe ok", "topology_probe_ok", false },
    { "topology probe fault", "topology_probe_fault", false },
    { "no upstream response", "no_upstream_response", false },
    { "sensor invalid", "sensor_invalid", false },
    { "first sample ok", "first_sample_ok", false },
    { "no valid sample", "no_valid_sample", false },
    { "busy responds", "busy_responds", false },
    { "no BUSY response", "no_busy_response", false },
    { "presence probe failed", "presence_probe_failed", false },
    { "blocked: 5V rail low", "blocked_5v_rail_low", false },
    { "init failed", "init_failed", false },
    { "start failed", "start_failed", true },
    { "sensor ok", "sensor_ok", false },
    { "upstream target assigned", "upstream_target_assigned", false },
    { "upstream topology fault", "upstream_topology_fault", false },
};

const char *hw_inventory_reason_code(hw_module_id_t id, const char *reason)
{
    (void)id;

    if (reason == NULL || reason[0] == '\0') {
        return "none";
    }

    for (size_t i = 0U; i < sizeof(s_reason_codes) / sizeof(s_reason_codes[0]); ++i) {
        const hw_inventory_reason_entry_t *entry = &s_reason_codes[i];
        if (entry->prefix) {
            if (strncmp(reason, entry->reason, strlen(entry->reason)) == 0) {
                return entry->code;
            }
        } else if (strcmp(reason, entry->reason) == 0) {
            return entry->code;
        }
    }

    return "unrecognized";
}
