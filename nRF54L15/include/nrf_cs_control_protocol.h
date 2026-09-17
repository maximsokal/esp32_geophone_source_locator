#ifndef NRF_CS_CONTROL_PROTOCOL_H
#define NRF_CS_CONTROL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_CS_CONTROL_EXECUTE_LENGTH 32U
#define NRF_CS_CONTROL_STATUS_REQUEST_LENGTH 32U
#define NRF_CS_CONTROL_RESULT_LENGTH 44U
#define NRF_CS_CONTROL_MESSAGE_EXECUTE 0x03U
#define NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST 0x04U
#define NRF_CS_CONTROL_MESSAGE_RESULT 0x81U

typedef enum {
    NRF_CS_CONTROL_STATE_IDLE = 0,
    NRF_CS_CONTROL_STATE_RUNNING = 1,
    NRF_CS_CONTROL_STATE_COMPLETE = 2,
    NRF_CS_CONTROL_STATE_FAILED = 3,
} nrf_cs_control_state_t;

typedef enum {
    NRF_CS_CONTROL_RESULT_OK = 0,
    NRF_CS_CONTROL_RESULT_PENDING = 1,
    NRF_CS_CONTROL_RESULT_BUSY = 2,
    NRF_CS_CONTROL_RESULT_STALE = 3,
    NRF_CS_CONTROL_RESULT_INVALID = 4,
    NRF_CS_CONTROL_RESULT_INTERNAL_ERROR = 5,
} nrf_cs_control_command_result_t;

typedef struct { uint32_t request_id; nrf_cs_step_t step; } nrf_cs_control_request_t;
typedef struct {
    uint32_t request_id;
    nrf_cs_step_t step;
    uint8_t state;
    uint8_t command_result;
    nrf_cs_measurement_t measurement;
} nrf_cs_control_result_t;

bool nrf_cs_control_step_equal(const nrf_cs_step_t *a, const nrf_cs_step_t *b);
int nrf_cs_control_validate_request(const nrf_cs_control_request_t *request);
int nrf_cs_control_encode_execute(const nrf_cs_control_request_t *request, uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH]);
int nrf_cs_control_decode_execute(const uint8_t *frame, size_t length, nrf_cs_control_request_t *request);
int nrf_cs_control_encode_status_request(const nrf_cs_control_request_t *request, uint8_t frame[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH]);
int nrf_cs_control_decode_status_request(const uint8_t *frame, size_t length, nrf_cs_control_request_t *request);
int nrf_cs_control_encode_result(const nrf_cs_control_result_t *result, uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH]);
int nrf_cs_control_decode_result(const uint8_t *frame, size_t length, nrf_cs_control_result_t *result);

#ifdef __cplusplus
}
#endif
#endif
