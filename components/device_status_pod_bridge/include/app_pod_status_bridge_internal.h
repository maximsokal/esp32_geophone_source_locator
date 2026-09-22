#pragma once

/*
 * One bridge publication cycle is exposed through the component-internal
 * seam so the host harness can execute the same runtime mapping without
 * pretending that a FreeRTOS scheduler or ESP32 peripherals ran.
 */
void app_pod_status_bridge_publish_cycle(void);
