#pragma once

#include <stdint.h>

#include "esp_wifi.h"

typedef enum {
    WIFI_MANAGER_RECONNECT_TRANSIENT = 0,
    WIFI_MANAGER_RECONNECT_PERSISTENT
} wifi_manager_reconnect_class_t;

typedef struct {
    uint32_t delay_ms;
    wifi_manager_reconnect_class_t reconnect_class;
    wifi_err_reason_t reason;
    uint32_t generation;
} wifi_manager_reconnect_decision_t;

wifi_manager_reconnect_decision_t wifi_manager_reconnect_policy_decide(
    wifi_err_reason_t reason,
    uint32_t consecutive_failure_count,
    uint32_t same_reason_streak,
    uint32_t generation
);
