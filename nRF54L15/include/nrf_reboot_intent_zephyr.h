#ifndef NRF_REBOOT_INTENT_ZEPHYR_H
#define NRF_REBOOT_INTENT_ZEPHYR_H

#include <stdint.h>

/* Stage of an intentional software reset, retained separately from CPU faults. */
typedef enum {
    NRF_REBOOT_INTENT_READDRESS = 1,
    NRF_REBOOT_INTENT_ASSIGN_SENSOR = 2,
    NRF_REBOOT_INTENT_ASSIGN_CS_PAIR = 3,
    NRF_REBOOT_INTENT_ASSIGN_CS_CONTROL = 4,
    NRF_REBOOT_INTENT_ASSIGN_RETRY_QUEUE = 5,
} nrf_reboot_intent_stage_t;

void nrf_reboot_intent_zephyr_record(nrf_reboot_intent_stage_t stage, int error_code);
void nrf_reboot_intent_zephyr_report_pending(void);

#endif
