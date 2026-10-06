#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware module inventory (MAX-383).
 *
 * One registry that records which fixed modules of the main enclosure were
 * physically detected, so boot policy, TFT and WebUI work from what answered
 * instead of assuming everything is mounted. Remote sensors are NOT tracked
 * here; they stay in nrf_topology_service.
 *
 * ABSENT means the module gave no response at all (I2C address NACK, no BUSY
 * edge, no SD card response). FAULT means something responded but is unusable
 * (bus stuck/timeout, bad chip id, filesystem error). Only the owner of a
 * module reports its state; consumers only read.
 */

typedef enum {
    HW_MODULE_SD_CARD = 0,
    HW_MODULE_AD7606,
    HW_MODULE_SENSOR_MUX,
    HW_MODULE_MAIN_XIAO,
    HW_MODULE_BAROMETER,
    HW_MODULE_POWER_MONITOR,
    HW_MODULE_COUNT
} hw_module_id_t;

typedef enum {
    HW_STATE_UNKNOWN = 0,
    HW_STATE_PRESENT,
    HW_STATE_ABSENT,
    HW_STATE_FAULT,
    HW_STATE_DEGRADED
} hw_module_state_t;

typedef enum {
    HW_REQ_OPTIONAL = 0,
    HW_REQ_REQUIRED
} hw_module_requirement_t;

#define HW_INVENTORY_REASON_LEN 48

typedef struct {
    hw_module_id_t id;
    hw_module_state_t state;
    hw_module_requirement_t requirement;
    char reason[HW_INVENTORY_REASON_LEN]; /* short ASCII, e.g. "no response" */
} hw_module_info_t;

/*
 * Reset every module to HW_STATE_UNKNOWN with its default requirement:
 * SD_CARD and AD7606 are REQUIRED; SENSOR_MUX, MAIN_XIAO, BAROMETER and
 * POWER_MONITOR are OPTIONAL. Safe to call more than once.
 */
void hw_inventory_init(void);

/* Record the state of one module. Out-of-range id is ignored. reason may be
 * NULL (stored as empty) and is truncated to HW_INVENTORY_REASON_LEN - 1. */
void hw_inventory_report(hw_module_id_t id, hw_module_state_t state, const char *reason);

/* Copy one module's record. Returns false for an out-of-range id or NULL out. */
bool hw_inventory_get(hw_module_id_t id, hw_module_info_t *out);

/* Copy up to cap records in id order; returns the number copied
 * (min(cap, HW_MODULE_COUNT)). A consistent snapshot under one lock. */
size_t hw_inventory_snapshot(hw_module_info_t *out, size_t cap);

/* Map a probe esp_err_t to a state: ESP_OK -> PRESENT, ESP_ERR_NOT_FOUND ->
 * ABSENT (address NACK, MAX-248), everything else (timeout, invalid state,
 * invalid response) -> FAULT. */
hw_module_state_t hw_inventory_classify_probe_error(esp_err_t err);

/* True when at least one REQUIRED module is ABSENT or FAULT; the first such
 * module id (lowest id) is stored in first_missing when non-NULL.
 * UNKNOWN modules do not count (they have not been probed yet). */
bool hw_inventory_required_missing(hw_module_id_t *first_missing);

/* Stable lowercase identifiers, "unknown" for out-of-range values:
 * "sd_card","ad7606","sensor_mux","main_xiao","barometer","power_monitor";
 * states "unknown","present","absent","fault","degraded". */
const char *hw_inventory_module_name(hw_module_id_t id);
const char *hw_inventory_state_name(hw_module_state_t state);

/* Log one line per module at INFO (tag "HW_INVENTORY"):
 * "HW_MODULE id=sd_card state=present req=required reason=...". */
void hw_inventory_log_summary(void);

#ifdef __cplusplus
}
#endif
