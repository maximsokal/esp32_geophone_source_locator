#pragma once

/*
 * One bridge publication cycle is exposed through the component-internal
 * seam so the host harness can execute the same runtime mapping without
 * pretending that a FreeRTOS scheduler or ESP32 peripherals ran. Firmware
 * calls it only from the bridge task; manual and host calls must be serialized.
 */
void app_pod_status_bridge_publish_cycle(void);

#if !defined(ESP_PLATFORM)
void app_pod_status_bridge_test_fail_next_scratch_allocations(unsigned count);
unsigned app_pod_status_bridge_test_scratch_allocation_attempts(void);
unsigned app_pod_status_bridge_test_scratch_allocation_count(void);
#endif
