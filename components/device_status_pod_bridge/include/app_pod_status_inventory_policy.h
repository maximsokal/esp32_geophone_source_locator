#pragma once

#include <stdbool.h>

#include "nrf_topology.h"

/* Reports SENSOR_MUX and MAIN_XIAO from a reconciled topology snapshot.
 * Returns false and leaves the registry unchanged before the first reconcile. */
bool app_pod_status_bridge_report_topology_inventory(
    const nrf_topology_snapshot_t *snapshot);
