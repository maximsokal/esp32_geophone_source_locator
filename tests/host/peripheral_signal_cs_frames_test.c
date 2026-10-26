/*
 * Channel Sounding control wire: every byte the ESP32 can send to a node and
 * every byte a node can answer. Finite-domain checks against the documented
 * frame layout (magic, version, type, length, request id, step, CRC16).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_control_protocol.h"
#include "nrf_upstream_protocol.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

static nrf_cs_control_request_t valid_request(void)
{
    nrf_cs_control_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = 0x01020304U;
    request.step.cycle_id = 7U;
    request.step.membership_generation = 3U;
    request.step.attempt_id = 0x11223344U;
    request.step.pair_index = 4U;
    request.step.attempt_number = 2U;
    request.step.initiator = 1U;
    request.step.reflector = 3U;
    request.step.timeout_ms = 500U;
    return request;
}

static nrf_cs_control_result_t valid_result(void)
{
    nrf_cs_control_request_t request = valid_request();
    nrf_cs_control_result_t result;
    memset(&result, 0, sizeof(result));
    result.request_id = request.request_id;
    result.step = request.step;
    result.state = NRF_CS_CONTROL_STATE_COMPLETE;
    result.command_result = NRF_CS_CONTROL_RESULT_OK;
    result.measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    result.measurement.distance_mm = 2345;
    result.measurement.confidence = 800U;
    result.measurement.provider_quality_flags = 0x000FU;
    return result;
}

static void request_round_trip_and_field_domain(void)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    nrf_cs_control_request_t decoded;
    nrf_cs_control_request_t request = valid_request();

    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    assert(frame[3] == NRF_CS_CONTROL_MESSAGE_EXECUTE);
    assert(nrf_cs_control_decode_execute(frame, sizeof(frame), &decoded) == 0);
    assert(decoded.request_id == request.request_id);
    assert(nrf_cs_control_step_equal(&decoded.step, &request.step));
    assert(decoded.step.timeout_ms == request.step.timeout_ms);

    /* The status request is the same layout under a different type. */
    assert(nrf_cs_control_encode_status_request(&request, frame) == 0);
    assert(frame[3] == NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST);
    assert(nrf_cs_control_decode_status_request(frame, sizeof(frame), &decoded) == 0);
    assert(nrf_cs_control_decode_execute(frame, sizeof(frame), &decoded) != 0);

    /* Boundary values that are valid. */
    request = valid_request();
    request.step.pair_index = NRF_CS_MAX_PAIRS - 1U;
    request.step.attempt_number = 255U;
    request.step.initiator = 0U;
    request.step.reflector = 254U;
    request.step.timeout_ms = UINT32_MAX;
    request.request_id = UINT32_MAX;
    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    assert(nrf_cs_control_decode_execute(frame, sizeof(frame), &decoded) == 0);
    assert(nrf_cs_control_step_equal(&decoded.step, &request.step));
    assert(decoded.step.timeout_ms == UINT32_MAX && decoded.request_id == UINT32_MAX);
    CASE_PASS("CS.CONTROL.request_round_trip");
}

static void invalid_request_fields_are_never_encoded(void)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    nrf_cs_control_request_t request;

#define EXPECT_REJECTED(mutation) \
    do { request = valid_request(); mutation; \
         assert(nrf_cs_control_validate_request(&request) != 0); \
         assert(nrf_cs_control_encode_execute(&request, frame) != 0); \
         assert(nrf_cs_control_encode_status_request(&request, frame) != 0); } while (0)

    EXPECT_REJECTED(request.request_id = 0U);
    EXPECT_REJECTED(request.step.cycle_id = 0U);
    EXPECT_REJECTED(request.step.membership_generation = 0U);
    EXPECT_REJECTED(request.step.attempt_id = 0U);
    EXPECT_REJECTED(request.step.attempt_number = 0U);
    EXPECT_REJECTED(request.step.timeout_ms = 0U);
    EXPECT_REJECTED(request.step.initiator = 0xFFU);
    EXPECT_REJECTED(request.step.reflector = 0xFFU);
    EXPECT_REJECTED(request.step.reflector = request.step.initiator);
    for (unsigned pair = NRF_CS_MAX_PAIRS; pair <= 255U; ++pair) {
        EXPECT_REJECTED(request.step.pair_index = (uint8_t)pair);
    }
#undef EXPECT_REJECTED

    request = valid_request();
    assert(nrf_cs_control_validate_request(NULL) != 0);
    assert(nrf_cs_control_encode_execute(NULL, frame) != 0);
    assert(nrf_cs_control_encode_execute(&request, NULL) != 0);
    CASE_PASS("CS.CONTROL.request_field_domain");
}

static void every_single_bit_flip_in_a_request_is_rejected(void)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    uint8_t mutated[NRF_CS_CONTROL_EXECUTE_LENGTH];
    nrf_cs_control_request_t decoded;
    nrf_cs_control_request_t request = valid_request();
    unsigned mutations = 0U;

    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    for (size_t byte = 0U; byte < sizeof(frame); ++byte) {
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            memcpy(mutated, frame, sizeof(frame));
            mutated[byte] ^= (uint8_t)(1U << bit);
            assert(nrf_cs_control_decode_execute(mutated, sizeof(mutated), &decoded) != 0);
            ++mutations;
        }
    }
    assert(mutations == 8U * NRF_CS_CONTROL_EXECUTE_LENGTH);
    CASE_PASS("CS.CONTROL.request_bit_flips");
}

static void only_the_exact_frame_length_decodes(void)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH + 16U];
    nrf_cs_control_request_t decoded;
    nrf_cs_control_request_t request = valid_request();

    memset(frame, 0, sizeof(frame));
    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    for (size_t length = 0U; length <= sizeof(frame); ++length) {
        const int result = nrf_cs_control_decode_execute(frame, length, &decoded);
        assert((result == 0) == (length == NRF_CS_CONTROL_EXECUTE_LENGTH));
    }
    assert(nrf_cs_control_decode_execute(NULL, NRF_CS_CONTROL_EXECUTE_LENGTH, &decoded) != 0);
    assert(nrf_cs_control_decode_execute(frame, NRF_CS_CONTROL_EXECUTE_LENGTH, NULL) != 0);
    CASE_PASS("CS.CONTROL.request_length_domain");
}

static void every_header_byte_is_checked_even_with_a_recomputed_crc(void)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    nrf_cs_control_request_t decoded;
    nrf_cs_control_request_t request = valid_request();

    /* A forged frame with a valid CRC must still fail the header checks. */
    for (size_t byte = 0U; byte < 10U; ++byte) {
        assert(nrf_cs_control_encode_execute(&request, frame) == 0);
        frame[byte] ^= 0x5AU;
        const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 30U);
        frame[30] = (uint8_t)crc;
        frame[31] = (uint8_t)(crc >> 8U);
        const int result = nrf_cs_control_decode_execute(frame, sizeof(frame), &decoded);
        /* Request-id bytes (6..9) change the id; that stays valid unless it becomes 0. */
        if (byte >= 6U) {
            assert(result == 0 && decoded.request_id != request.request_id);
        } else {
            assert(result != 0);
        }
    }

    /* A zero request id is rejected even with a valid CRC. */
    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    memset(&frame[6], 0, 4U);
    const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 30U);
    frame[30] = (uint8_t)crc;
    frame[31] = (uint8_t)(crc >> 8U);
    assert(nrf_cs_control_decode_execute(frame, sizeof(frame), &decoded) != 0);
    CASE_PASS("CS.CONTROL.request_header_checks");
}

static void result_round_trip_for_every_outcome_and_state(void)
{
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    nrf_cs_control_result_t decoded;
    unsigned accepted = 0U;

    for (unsigned state = 0U; state <= NRF_CS_CONTROL_STATE_FAILED; ++state) {
        for (unsigned command = 0U; command <= NRF_CS_CONTROL_RESULT_INTERNAL_ERROR; ++command) {
            for (unsigned outcome = 0U; outcome <= NRF_CS_OUTCOME_OUTLIER; ++outcome) {
                nrf_cs_control_result_t result = valid_result();
                result.state = (uint8_t)state;
                result.command_result = (uint8_t)command;
                result.measurement.outcome = (nrf_cs_outcome_t)outcome;
                if (outcome != NRF_CS_OUTCOME_SUCCESS) result.measurement.distance_mm = -1234;
                assert(nrf_cs_control_encode_result(&result, frame) == 0);
                assert(nrf_cs_control_decode_result(frame, sizeof(frame), &decoded) == 0);
                assert(decoded.state == state && decoded.command_result == command);
                assert(decoded.measurement.outcome == (nrf_cs_outcome_t)outcome);
                assert(decoded.measurement.distance_mm == result.measurement.distance_mm);
                assert(decoded.measurement.confidence == result.measurement.confidence);
                assert(decoded.measurement.provider_quality_flags ==
                       result.measurement.provider_quality_flags);
                ++accepted;
            }
        }
    }
    assert(accepted == 4U * 6U * 4U);
    CASE_PASS("CS.CONTROL.result_state_domain");
}

static void result_values_outside_the_domain_are_never_encoded(void)
{
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];

    for (unsigned state = NRF_CS_CONTROL_STATE_FAILED + 1U; state <= 255U; ++state) {
        nrf_cs_control_result_t result = valid_result();
        result.state = (uint8_t)state;
        assert(nrf_cs_control_encode_result(&result, frame) != 0);
    }
    for (unsigned command = NRF_CS_CONTROL_RESULT_INTERNAL_ERROR + 1U; command <= 255U; ++command) {
        nrf_cs_control_result_t result = valid_result();
        result.command_result = (uint8_t)command;
        assert(nrf_cs_control_encode_result(&result, frame) != 0);
    }
    for (unsigned outcome = NRF_CS_OUTCOME_OUTLIER + 1U; outcome <= 255U; ++outcome) {
        nrf_cs_control_result_t result = valid_result();
        result.measurement.outcome = (nrf_cs_outcome_t)outcome;
        assert(nrf_cs_control_encode_result(&result, frame) != 0);
    }
    /* Confidence 0..1000 is the whole valid range. */
    for (unsigned confidence = 0U; confidence <= NRF_CS_CONFIDENCE_MAX; confidence += 1U) {
        nrf_cs_control_result_t result = valid_result();
        result.measurement.confidence = (uint16_t)confidence;
        assert(nrf_cs_control_encode_result(&result, frame) == 0);
    }
    for (unsigned confidence = NRF_CS_CONFIDENCE_MAX + 1U; confidence <= 0xFFFFU; confidence += 257U) {
        nrf_cs_control_result_t result = valid_result();
        result.measurement.confidence = (uint16_t)confidence;
        assert(nrf_cs_control_encode_result(&result, frame) != 0);
    }
    /* A successful measurement can never carry a negative distance. */
    nrf_cs_control_result_t negative = valid_result();
    negative.measurement.distance_mm = -1;
    assert(nrf_cs_control_encode_result(&negative, frame) != 0);
    negative.measurement.distance_mm = INT32_MIN;
    assert(nrf_cs_control_encode_result(&negative, frame) != 0);
    negative.measurement.distance_mm = 0;
    assert(nrf_cs_control_encode_result(&negative, frame) == 0);
    negative.measurement.distance_mm = INT32_MAX;
    assert(nrf_cs_control_encode_result(&negative, frame) == 0);
    CASE_PASS("CS.CONTROL.result_value_domain");
}

static void result_wire_bytes_outside_the_domain_are_rejected_with_a_valid_crc(void)
{
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    nrf_cs_control_result_t decoded;
    nrf_cs_control_result_t result = valid_result();

    /* Corrupt one logical field, then recompute the CRC: the receiver must
     * still reject the value by itself, not only through the checksum. */
    const size_t offsets[] = {30U, 31U, 32U};
    const unsigned first_invalid[] = {NRF_CS_CONTROL_STATE_FAILED + 1U,
                                      NRF_CS_CONTROL_RESULT_INTERNAL_ERROR + 1U,
                                      NRF_CS_OUTCOME_OUTLIER + 1U};
    for (size_t field = 0U; field < 3U; ++field) {
        for (unsigned value = first_invalid[field]; value <= 255U; ++value) {
            assert(nrf_cs_control_encode_result(&result, frame) == 0);
            frame[offsets[field]] = (uint8_t)value;
            const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 42U);
            frame[42] = (uint8_t)crc;
            frame[43] = (uint8_t)(crc >> 8U);
            assert(nrf_cs_control_decode_result(frame, sizeof(frame), &decoded) != 0);
        }
    }
    /* Confidence above the maximum and negative distance on success. */
    assert(nrf_cs_control_encode_result(&result, frame) == 0);
    frame[34] = (uint8_t)((NRF_CS_CONFIDENCE_MAX + 1U) & 0xFFU);
    frame[35] = (uint8_t)((NRF_CS_CONFIDENCE_MAX + 1U) >> 8U);
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 42U);
    frame[42] = (uint8_t)crc;
    frame[43] = (uint8_t)(crc >> 8U);
    assert(nrf_cs_control_decode_result(frame, sizeof(frame), &decoded) != 0);

    assert(nrf_cs_control_encode_result(&result, frame) == 0);
    memset(&frame[38], 0xFF, 4U); /* -1 mm */
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 42U);
    frame[42] = (uint8_t)crc;
    frame[43] = (uint8_t)(crc >> 8U);
    assert(nrf_cs_control_decode_result(frame, sizeof(frame), &decoded) != 0);
    CASE_PASS("CS.CONTROL.result_wire_domain");
}

static void every_single_bit_flip_in_a_result_is_rejected(void)
{
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    uint8_t mutated[NRF_CS_CONTROL_RESULT_LENGTH];
    nrf_cs_control_result_t decoded;
    nrf_cs_control_result_t result = valid_result();

    assert(nrf_cs_control_encode_result(&result, frame) == 0);
    for (size_t byte = 0U; byte < sizeof(frame); ++byte) {
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            memcpy(mutated, frame, sizeof(frame));
            mutated[byte] ^= (uint8_t)(1U << bit);
            assert(nrf_cs_control_decode_result(mutated, sizeof(mutated), &decoded) != 0);
        }
    }
    for (size_t length = 0U; length <= sizeof(frame) + 8U; ++length) {
        const int decoded_ok = nrf_cs_control_decode_result(frame, length, &decoded);
        assert((decoded_ok == 0) == (length == NRF_CS_CONTROL_RESULT_LENGTH));
    }
    CASE_PASS("CS.CONTROL.result_bit_flips");
}

static void negative_distance_survives_the_wire_for_failed_outcomes(void)
{
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    nrf_cs_control_result_t decoded;
    const int32_t samples[] = {INT32_MIN, INT32_MIN + 1, -65536, -256, -2, -1, 0, 1, 255, 65535,
                               INT32_MAX - 1, INT32_MAX};

    for (size_t i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        nrf_cs_control_result_t result = valid_result();
        result.measurement.outcome = NRF_CS_OUTCOME_TIMEOUT;
        result.measurement.distance_mm = samples[i];
        assert(nrf_cs_control_encode_result(&result, frame) == 0);
        assert(nrf_cs_control_decode_result(frame, sizeof(frame), &decoded) == 0);
        assert(decoded.measurement.distance_mm == samples[i]);
    }
    CASE_PASS("CS.CONTROL.signed_distance_wire");
}

int main(void)
{
    /* Progress lines must survive a crash in a later case. */
    setvbuf(stdout, NULL, _IONBF, 0);
    request_round_trip_and_field_domain();
    invalid_request_fields_are_never_encoded();
    every_single_bit_flip_in_a_request_is_rejected();
    only_the_exact_frame_length_decodes();
    every_header_byte_is_checked_even_with_a_recomputed_crc();
    result_round_trip_for_every_outcome_and_state();
    result_values_outside_the_domain_are_never_encoded();
    result_wire_bytes_outside_the_domain_are_rejected_with_a_valid_crc();
    every_single_bit_flip_in_a_result_is_rejected();
    negative_distance_survives_the_wire_for_failed_outcomes();
    return 0;
}
