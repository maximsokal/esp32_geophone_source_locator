#include <assert.h>
#include <string.h>

#include "nrf_node_state.h"
#include "nrf_remote_sensor_runtime.h"

#ifndef NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES
#define NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES 3U
#endif

typedef struct {
    int imu_init_calls;
    int env_init_calls;
    int imu_read_calls;
    int env_read_calls;
    int imu_init_result;
    int env_init_result;
    int imu_read_result;
    int env_read_result;
    nrf_imu_sample_t imu_sample;
    nrf_env_sample_t env_sample;
} fake_sensor_context_t;

static int fake_init_imu(void *context)
{
    fake_sensor_context_t *fake = (fake_sensor_context_t *)context;
    fake->imu_init_calls++;
    return fake->imu_init_result;
}

static int fake_init_env(void *context)
{
    fake_sensor_context_t *fake = (fake_sensor_context_t *)context;
    fake->env_init_calls++;
    return fake->env_init_result;
}

static int fake_read_imu(void *context, nrf_imu_sample_t *sample)
{
    fake_sensor_context_t *fake = (fake_sensor_context_t *)context;
    fake->imu_read_calls++;
    *sample = fake->imu_sample;
    return fake->imu_read_result;
}

static int fake_read_env(void *context, nrf_env_sample_t *sample)
{
    fake_sensor_context_t *fake = (fake_sensor_context_t *)context;
    fake->env_read_calls++;
    *sample = fake->env_sample;
    return fake->env_read_result;
}

static fake_sensor_context_t make_fake(void)
{
    fake_sensor_context_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.imu_sample.accel_mg[2] = 1001;
    fake.imu_sample.gyro_mdps[1] = -125;
    fake.imu_sample.temperature_milli_c = 24125;
    fake.env_sample.temperature_milli_c = 24250;
    fake.env_sample.pressure_pa = 100912U;
    fake.env_sample.humidity_milli_percent = 48125U;
    return fake;
}

static nrf_remote_sensor_ops_t make_ops(fake_sensor_context_t *fake)
{
    const nrf_remote_sensor_ops_t ops = {
        .init_imu = fake_init_imu,
        .read_imu = fake_read_imu,
        .imu_context = fake,
        .init_env = fake_init_env,
        .read_env = fake_read_env,
        .env_context = fake,
    };
    return ops;
}

static nrf_node_state_t make_ready_node(nrf_node_role_t role)
{
    nrf_node_state_t state;
    nrf_node_state_init(&state);
    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);
    if (role != NRF_NODE_ROLE_UNASSIGNED) {
        assert(nrf_node_state_assign_role(&state, role) == NRF_NODE_STATE_OK);
    }
    return state;
}

static void test_main_and_unassigned_never_initialize_remote_hardware(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    nrf_node_state_t main_state = make_ready_node(NRF_NODE_ROLE_MAIN);
    assert(nrf_remote_sensor_runtime_prepare(&main_state, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_SKIPPED);
    assert(nrf_remote_sensor_runtime_refresh(&main_state, &runtime) ==
           NRF_SENSOR_SNAPSHOT_SKIPPED);

    nrf_node_state_t unassigned = make_ready_node(NRF_NODE_ROLE_UNASSIGNED);
    assert(nrf_remote_sensor_runtime_prepare(&unassigned, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_SKIPPED);
    assert(nrf_remote_sensor_runtime_refresh(&unassigned, &runtime) ==
           NRF_SENSOR_SNAPSHOT_SKIPPED);

    assert(fake.imu_init_calls == 0);
    assert(fake.env_init_calls == 0);
    assert(fake.imu_read_calls == 0);
    assert(fake.env_read_calls == 0);
}

static void test_remote_prepare_is_idempotent_and_refreshes_both_sources(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(runtime.imu_ready);
    assert(runtime.env_ready);
    assert(fake.imu_init_calls == 1);
    assert(fake.env_init_calls == 1);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(fake.imu_init_calls == 1);
    assert(fake.env_init_calls == 1);

    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_OK);
    assert(runtime.snapshot.valid_mask ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
    assert(runtime.snapshot.fault_mask == 0U);
    assert(runtime.snapshot.imu.accel_mg[2] == 1001);
    assert(runtime.snapshot.env.pressure_pa == 100912U);
    assert(fake.imu_read_calls == 1);
    assert(fake.env_read_calls == 1);
}

static void test_failed_sensor_can_retry_without_reinitializing_good_sensor(void)
{
    fake_sensor_context_t fake = make_fake();
    fake.env_init_result = -7;
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_PARTIAL);
    assert(runtime.imu_ready);
    assert(!runtime.env_ready);
    assert(runtime.env_init_result == -7);

    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(runtime.snapshot.valid_mask == NRF_SENSOR_VALID_IMU);
    assert(runtime.snapshot.fault_mask == NRF_SENSOR_VALID_ENV);
    assert(fake.imu_read_calls == 1);
    assert(fake.env_read_calls == 0);

    fake.env_init_result = 0;
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(fake.imu_init_calls == 1);
    assert(fake.env_init_calls == 2);
    assert(runtime.env_ready);

    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_OK);
    assert(fake.imu_read_calls == 2);
    assert(fake.env_read_calls == 1);
}

static void test_failed_read_clears_stale_payload_validity(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_OK);
    assert(runtime.snapshot.env.pressure_pa == 100912U);

    fake.env_read_result = -8;
    fake.imu_sample.accel_mg[2] = 999;
    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(runtime.snapshot.sequence == 2U);
    assert(runtime.snapshot.valid_mask == NRF_SENSOR_VALID_IMU);
    assert(runtime.snapshot.fault_mask == NRF_SENSOR_VALID_ENV);
    assert(runtime.snapshot.imu.accel_mg[2] == 999);
    assert(runtime.snapshot.env.pressure_pa == 0U);
}

static void test_unavailable_remote_does_not_touch_sensors(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_node_state_mark_unavailable(&remote) == NRF_NODE_STATE_OK);
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY);
    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_ERR_NOT_READY);
    assert(fake.imu_init_calls == 0);
    assert(fake.env_init_calls == 0);
    assert(fake.imu_read_calls == 0);
    assert(fake.env_read_calls == 0);
}

static void test_missing_sensor_ops_report_no_data_without_crash(void)
{
    nrf_remote_sensor_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA);
    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_ERR_NO_DATA);
    assert(runtime.snapshot.fault_mask ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
}

static void test_argument_validation(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(NULL, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT);
    assert(nrf_remote_sensor_runtime_prepare(&remote, NULL, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT);
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, NULL) ==
           NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT);
    assert(nrf_remote_sensor_runtime_refresh(NULL, &runtime) ==
           NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT);
    assert(nrf_remote_sensor_runtime_refresh(&remote, NULL) ==
           NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT);
}

static void test_ready_source_is_reinitialized_after_consecutive_read_failures(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(fake.imu_init_calls == 1 && fake.env_init_calls == 1);

    /* The sensor lost power: reads fail until it is initialized again. */
    fake.imu_read_result = -4;
    for (unsigned attempt = 1U; attempt < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES; ++attempt) {
        (void)nrf_remote_sensor_runtime_refresh(&remote, &runtime);
        assert(runtime.imu_ready);
        assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
               NRF_REMOTE_SENSOR_PREPARE_OK);
        assert(fake.imu_init_calls == 1);
    }
    (void)nrf_remote_sensor_runtime_refresh(&remote, &runtime);
    assert(!runtime.imu_ready);
    assert(runtime.env_ready);
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(fake.imu_init_calls == 2);
    assert(fake.env_init_calls == 1);
    assert(runtime.imu_ready);

    /* A good read in between resets the streak. */
    fake.imu_read_result = 0;
    (void)nrf_remote_sensor_runtime_refresh(&remote, &runtime);
    fake.imu_read_result = -4;
    for (unsigned attempt = 1U; attempt < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES; ++attempt) {
        (void)nrf_remote_sensor_runtime_refresh(&remote, &runtime);
    }
    assert(runtime.imu_ready);
}

static void test_failed_reinit_is_retried_on_every_prepare(void)
{
    fake_sensor_context_t fake = make_fake();
    const nrf_remote_sensor_ops_t ops = make_ops(&fake);
    nrf_node_state_t remote = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_remote_sensor_runtime_t runtime;
    nrf_remote_sensor_runtime_init(&runtime);

    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    fake.env_read_result = -4;
    for (unsigned attempt = 0U; attempt < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES; ++attempt) {
        (void)nrf_remote_sensor_runtime_refresh(&remote, &runtime);
    }
    assert(!runtime.env_ready);

    fake.env_init_result = -7;
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_PARTIAL);
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_PARTIAL);
    assert(fake.env_init_calls == 3);

    fake.env_init_result = 0;
    fake.env_read_result = 0;
    assert(nrf_remote_sensor_runtime_prepare(&remote, &ops, &runtime) ==
           NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(nrf_remote_sensor_runtime_refresh(&remote, &runtime) ==
           NRF_SENSOR_SNAPSHOT_OK);
}

int main(void)
{
    test_ready_source_is_reinitialized_after_consecutive_read_failures();
    test_failed_reinit_is_retried_on_every_prepare();
    test_main_and_unassigned_never_initialize_remote_hardware();
    test_remote_prepare_is_idempotent_and_refreshes_both_sources();
    test_failed_sensor_can_retry_without_reinitializing_good_sensor();
    test_failed_read_clears_stale_payload_validity();
    test_unavailable_remote_does_not_touch_sensors();
    test_missing_sensor_ops_report_no_data_without_crash();
    test_argument_validation();
    return 0;
}
