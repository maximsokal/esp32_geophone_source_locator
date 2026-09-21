#include "nrf_topology_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NRF_CS_ACCURACY_HIL_VERSION 1U
#define NRF_CS_ACCURACY_HIL_REPEATS 20U
#define NRF_CS_ACCURACY_HIL_TOPOLOGY_TIMEOUT_MS 30000U
#define NRF_CS_ACCURACY_HIL_CYCLE_TIMEOUT_MS 20000U
#define NRF_CS_ACCURACY_HIL_POLL_MS 50U

static void emit_sample(
    uint32_t cycle_id,
    uint8_t pair_index,
    uint8_t node_a,
    uint8_t node_b,
    nrf_cs_pair_state_t state,
    nrf_cs_failure_reason_t last_failure,
    int32_t distance_mm,
    uint16_t confidence,
    uint16_t provider_quality_flags,
    uint16_t scheduler_quality_flags,
    uint16_t attempts,
    uint8_t valid_samples,
    int error_code)
{
    printf(
        "NRF_CS_ACCURACY_HIL_SAMPLE "
        "{\"cycle_id\":%lu,\"pair_index\":%u,\"pair_count\":1,"
        "\"node_a\":%u,\"node_b\":%u,\"state\":%u,"
        "\"last_failure\":%u,\"distance_mm\":%ld,"
        "\"confidence\":%u,\"provider_quality_flags\":%u,"
        "\"scheduler_quality_flags\":%u,\"attempts\":%u,"
        "\"valid_samples\":%u,\"error_code\":%d}\n",
        (unsigned long)cycle_id,
        (unsigned int)pair_index,
        (unsigned int)node_a,
        (unsigned int)node_b,
        (unsigned int)state,
        (unsigned int)last_failure,
        (long)distance_mm,
        (unsigned int)confidence,
        (unsigned int)provider_quality_flags,
        (unsigned int)scheduler_quality_flags,
        (unsigned int)attempts,
        (unsigned int)valid_samples,
        error_code);
    fflush(stdout);
}

static bool find_two_node_fixture(
    uint8_t *main_node_id,
    uint8_t *remote_node_id)
{
    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(NRF_CS_ACCURACY_HIL_TOPOLOGY_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        nrf_topology_snapshot_t topology;
        if (nrf_topology_service_get_snapshot(&topology) == ESP_OK) {
            uint8_t assigned_remotes = 0U;
            uint8_t candidate_remote = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
            for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
                if (topology.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
                    assigned_remotes++;
                    candidate_remote = topology.remotes[channel].node_id;
                }
            }

            if (topology.main.state == NRF_TOPOLOGY_NODE_ASSIGNED &&
                assigned_remotes == 1U) {
                *main_node_id = topology.main.node_id;
                *remote_node_id = candidate_remote;
                printf(
                    "NRF_CS_ACCURACY_HIL_TOPOLOGY main=%u remote=%u assigned=2\n",
                    (unsigned int)*main_node_id,
                    (unsigned int)*remote_node_id);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }

    return false;
}

static bool wait_for_cycle(uint32_t *cycle_id)
{
    const TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(NRF_CS_ACCURACY_HIL_CYCLE_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        nrf_cs_cycle_snapshot_t snapshot;
        if (nrf_topology_service_get_cs_cycle(&snapshot) == ESP_OK) {
            if (snapshot.complete && !snapshot.active) {
                *cycle_id = snapshot.cycle_id;
                for (size_t pair_index = 0U; pair_index < snapshot.pair_count; ++pair_index) {
                    const nrf_cs_pair_result_t *pair = &snapshot.pairs[pair_index];
                    emit_sample(
                        snapshot.cycle_id,
                        (uint8_t)pair_index,
                        pair->node_a,
                        pair->node_b,
                        pair->state,
                        pair->last_failure,
                        pair->distance_mm,
                        pair->confidence,
                        pair->provider_quality_flags,
                        pair->scheduler_quality_flags,
                        pair->attempts,
                        pair->valid_samples,
                        0);
                }
                return snapshot.pair_count != 0U;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NRF_CS_ACCURACY_HIL_POLL_MS));
    }

    return false;
}

void app_main(void)
{
    printf(
        "NRF_CS_ACCURACY_HIL_READY version=%u repeats=%u\n",
        NRF_CS_ACCURACY_HIL_VERSION,
        NRF_CS_ACCURACY_HIL_REPEATS);

    const esp_err_t start_result = nrf_topology_service_start();
    if (start_result != ESP_OK) {
        printf(
            "NRF_CS_ACCURACY_HIL_RESULT: BLOCKED reason=topology_start err=%s\n",
            esp_err_to_name(start_result));
        return;
    }

    uint8_t main_node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    uint8_t remote_node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    if (!find_two_node_fixture(&main_node_id, &remote_node_id)) {
        printf(
            "NRF_CS_ACCURACY_HIL_RESULT: BLOCKED "
            "reason=expected_one_main_one_remote_not_ready\n");
        nrf_topology_service_stop();
        return;
    }

    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 500U,
        .max_attempts = 3U,
        /* Report low-confidence results; the host runner evaluates the gate. */
        .min_confidence = 0U,
    };

    for (uint32_t repeat = 0U; repeat < NRF_CS_ACCURACY_HIL_REPEATS; ++repeat) {
        const esp_err_t request_result = nrf_topology_service_request_cs_cycle(&policy);
        if (request_result != ESP_OK) {
            emit_sample(
                repeat,
                0U,
                main_node_id,
                remote_node_id,
                NRF_CS_PAIR_MISSING,
                NRF_CS_FAILURE_TRANSPORT,
                -1,
                0U,
                0U,
                NRF_CS_QUALITY_TRANSPORT_ERROR,
                0U,
                0U,
                request_result);
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        uint32_t cycle_id = repeat;
        if (!wait_for_cycle(&cycle_id)) {
            emit_sample(
                cycle_id,
                0U,
                main_node_id,
                remote_node_id,
                NRF_CS_PAIR_MISSING,
                NRF_CS_FAILURE_TIMEOUT,
                -1,
                0U,
                0U,
                NRF_CS_QUALITY_TIMEOUT,
                0U,
                0U,
                ESP_ERR_TIMEOUT);
        }
        vTaskDelay(pdMS_TO_TICKS(100U));
    }

    printf("NRF_CS_ACCURACY_HIL_END\n");
    fflush(stdout);
    nrf_topology_service_stop();
}
