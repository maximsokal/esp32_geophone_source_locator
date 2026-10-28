#include "nrf_remote_sensor_runtime_zephyr.h"

#include <string.h>
#include <zephyr/kernel.h>

#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"

static int nrf_remote_sensor_zephyr_init_imu(void *context)
{
    nrf_bmi160_t *device = (nrf_bmi160_t *)context;
    return (int)nrf_bmi160_zephyr_init_remote(device);
}

static int nrf_remote_sensor_zephyr_init_env(void *context)
{
    nrf_bme280_t *device = (nrf_bme280_t *)context;
    return (int)nrf_bme280_zephyr_init_remote(device);
}

static int nrf_remote_sensor_zephyr_step_imu(void *context)
{
    nrf_remote_sensor_runtime_zephyr_t *runtime = context;
    if (!runtime->imu_init_active) {
        runtime->imu_address_index = 0U;
        runtime->imu_best_result = NRF_BMI160_ERR_BUS;
        const nrf_bmi160_result_t result = nrf_bmi160_zephyr_begin_address(
            &runtime->bmi160, NRF_BMI160_DEFAULT_ADDRESS);
        if (result != NRF_BMI160_PENDING) return result;
        runtime->imu_init_active = true;
        return NRF_REMOTE_SENSOR_INIT_PENDING;
    }
    const nrf_bmi160_result_t result = nrf_bmi160_zephyr_step(
        &runtime->bmi160, (uint64_t)k_uptime_get());
    if (result == NRF_BMI160_PENDING) return NRF_REMOTE_SENSOR_INIT_PENDING;
    if (result == NRF_BMI160_OK) {
        runtime->imu_init_active = false;
        return 0;
    }
    if (runtime->imu_address_index == 0U) {
        runtime->imu_best_result = result;
        runtime->imu_address_index = 1U;
        const nrf_bmi160_result_t next = nrf_bmi160_zephyr_begin_address(
            &runtime->bmi160, NRF_BMI160_ALTERNATE_ADDRESS);
        if (next == NRF_BMI160_PENDING) return NRF_REMOTE_SENSOR_INIT_PENDING;
        runtime->imu_init_active = false;
        return runtime->imu_best_result == NRF_BMI160_ERR_BUS ? next : runtime->imu_best_result;
    }
    runtime->imu_init_active = false;
    return runtime->imu_best_result == NRF_BMI160_ERR_BUS ? result : runtime->imu_best_result;
}

static int nrf_remote_sensor_zephyr_step_env(void *context)
{
    nrf_remote_sensor_runtime_zephyr_t *runtime = context;
    if (!runtime->env_init_active) {
        const nrf_bme280_result_t result = nrf_bme280_zephyr_begin_remote(&runtime->bme280);
        if (result != NRF_BME280_PENDING) return result;
        runtime->env_init_active = true;
        return NRF_REMOTE_SENSOR_INIT_PENDING;
    }
    const nrf_bme280_result_t result = nrf_bme280_zephyr_step(&runtime->bme280, (uint64_t)k_uptime_get());
    if (result != NRF_BME280_PENDING) runtime->env_init_active = false;
    return result;
}

void nrf_remote_sensor_runtime_zephyr_init(
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    nrf_remote_sensor_runtime_init(&runtime->core);
}

void nrf_remote_sensor_runtime_zephyr_note_imu_read(
    nrf_remote_sensor_runtime_zephyr_t *runtime, bool faulted)
{
    if (runtime == NULL) return;
    const bool was_ready = runtime->core.imu_ready;
    nrf_remote_sensor_runtime_note_imu_read(&runtime->core, faulted);
    if (runtime->bmi160.state == NRF_BMI160_RECOVER) runtime->core.imu_ready = false;
    if (was_ready && !runtime->core.imu_ready) {
        runtime->bmi160.initialized = false;
        runtime->bmi160.state = NRF_BMI160_RECOVER;
    }
    if (runtime->bmi160.state == NRF_BMI160_RECOVER &&
        nrf_bmi160_zephyr_state() != NRF_BMI160_RECOVER)
        nrf_bmi160_zephyr_note_recover();
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (node_state == NULL || runtime == NULL) {
        return NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT;
    }

    const nrf_remote_sensor_ops_t ops = {
        .now_ms = (uint64_t)k_uptime_get(),
        .init_imu = nrf_remote_sensor_zephyr_init_imu,
        .read_imu = nrf_bmi160_read_imu,
        .imu_context = &runtime->bmi160,
        .init_env = nrf_remote_sensor_zephyr_init_env,
        .read_env = nrf_bme280_read_env,
        .env_context = &runtime->bme280,
    };

    return nrf_remote_sensor_runtime_prepare(
        node_state,
        &ops,
        &runtime->core);
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare_cooperative(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (node_state == NULL || runtime == NULL) return NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT;
    const uint64_t now = (uint64_t)k_uptime_get();
    const nrf_remote_sensor_ops_t ops = {
        .now_ms = now, .init_imu = nrf_remote_sensor_zephyr_step_imu,
        .read_imu = nrf_bmi160_read_imu, .imu_context = runtime,
        .init_env = nrf_remote_sensor_zephyr_step_env,
        .read_env = nrf_bme280_read_env, .env_context = runtime,
    };
    const nrf_remote_sensor_prepare_result_t result = nrf_remote_sensor_runtime_prepare(node_state, &ops, &runtime->core);
    if (runtime->core.imu_ready) runtime->core.sources.imu_context = &runtime->bmi160;
    else if (runtime->imu_init_active) {
        const uint64_t due = runtime->bmi160.prepare_due_ms;
        runtime->core.imu_retry_after_ms = due > now ? due : (now == UINT64_MAX ? now : now + 1U);
    }
    if (runtime->core.env_ready) runtime->core.sources.env_context = &runtime->bme280;
    else if (runtime->env_init_active) {
        const uint64_t due = runtime->bme280.prepare_due_ms;
        runtime->core.env_retry_after_ms = due > now ? due : (now == UINT64_MAX ? now : now + 1U);
    }
    return result;
}

nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_begin_env(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms)
{
    if (node_state == NULL || runtime == NULL || node_state->lifecycle != NRF_NODE_LIFECYCLE_READY ||
        node_state->role != NRF_NODE_ROLE_REMOTE) return NRF_BME280_ERR_ARGUMENT;
    if (runtime->bme280.conversion_pending) return NRF_BME280_PENDING;
    runtime->env_prepared = false;
    memset(&runtime->prepared_env, 0, sizeof(runtime->prepared_env));
    if (!runtime->core.env_ready) return NRF_BME280_OK;
    runtime->prepared_env_result = nrf_bme280_start_conversion(&runtime->bme280, now_ms);
    if (runtime->prepared_env_result != NRF_BME280_OK) {
        runtime->env_prepared = true;
        return NRF_BME280_OK;
    }
    return NRF_BME280_PENDING;
}

nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_poll_env(
    nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL) return NRF_BME280_ERR_ARGUMENT;
    if (!runtime->bme280.conversion_pending) return NRF_BME280_OK;
    const nrf_bme280_result_t result = nrf_bme280_poll_conversion(
        &runtime->bme280, now_ms, &runtime->prepared_env);
    if (runtime->bme280.state == NRF_BME280_RECOVER &&
        nrf_bme280_zephyr_state() != NRF_BME280_RECOVER)
        nrf_bme280_zephyr_note_recover();
    if (result == NRF_BME280_PENDING) return result;
    runtime->prepared_env_result = result;
    runtime->env_prepared = true;
    return NRF_BME280_OK;
}

static int nrf_remote_sensor_prepared_env(void *context, nrf_env_sample_t *sample)
{
    const nrf_remote_sensor_runtime_zephyr_t *runtime = context;
    *sample = runtime->prepared_env;
    return runtime->prepared_env_result;
}

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (runtime == NULL) {
        return NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT;
    }
    if (runtime->bme280.conversion_pending) {
        return NRF_SENSOR_SNAPSHOT_ERR_NOT_READY;
    }

    const bool imu_was_ready = runtime->core.imu_ready;
    const bool env_was_ready = runtime->core.env_ready;
    const nrf_read_env_fn original_read = runtime->core.sources.read_env;
    void *const original_context = runtime->core.sources.env_context;
    if (runtime->env_prepared) {
        runtime->core.sources.read_env = nrf_remote_sensor_prepared_env;
        runtime->core.sources.env_context = runtime;
    }
    const nrf_sensor_snapshot_result_t result =
        nrf_remote_sensor_runtime_refresh(node_state, &runtime->core);
    runtime->core.sources.read_env = original_read;
    runtime->core.sources.env_context = original_context;
    runtime->env_prepared = false;
    if (imu_was_ready && runtime->bmi160.state == NRF_BMI160_RECOVER)
        runtime->core.imu_ready = false;
    if (env_was_ready && runtime->bme280.state == NRF_BME280_RECOVER)
        runtime->core.env_ready = false;
    if (imu_was_ready && !runtime->core.imu_ready) {
        runtime->bmi160.initialized = false;
        runtime->bmi160.state = NRF_BMI160_RECOVER;
    }
    if (runtime->bmi160.state == NRF_BMI160_RECOVER &&
        nrf_bmi160_zephyr_state() != NRF_BMI160_RECOVER)
        nrf_bmi160_zephyr_note_recover();
    if (env_was_ready && !runtime->core.env_ready) {
        runtime->bme280.initialized = false;
        runtime->bme280.state = NRF_BME280_RECOVER;
        nrf_bme280_zephyr_note_recover();
    }
    return result;
}
