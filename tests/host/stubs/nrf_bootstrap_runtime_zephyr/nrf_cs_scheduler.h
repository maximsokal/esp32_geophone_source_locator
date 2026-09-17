#ifndef NRF_CS_SCHEDULER_H
#define NRF_CS_SCHEDULER_H
#include <stdint.h>
typedef uint8_t nrf_cs_node_id_t;
typedef enum {
    NRF_CS_OUTCOME_SUCCESS = 0,
    NRF_CS_OUTCOME_TIMEOUT,
    NRF_CS_OUTCOME_TRANSPORT_ERROR,
    NRF_CS_OUTCOME_OUTLIER,
} nrf_cs_outcome_t;
typedef struct {
    nrf_cs_outcome_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
} nrf_cs_measurement_t;
typedef struct {
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t attempt_id;
    uint8_t pair_index;
    uint8_t attempt_number;
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
    uint32_t timeout_ms;
} nrf_cs_step_t;
#endif
