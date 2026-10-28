#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_node_state.h"
#include "nrf_sensor_snapshot.h"

typedef struct {
    int imu_calls;
    int env_calls;
    int imu_result;
    int env_result;
    nrf_imu_sample_t imu_sample;
    nrf_env_sample_t env_sample;
} fake_sources_context_t;

static int fake_read_imu(void *context, nrf_imu_sample_t *sample)
{
    fake_sources_context_t *fake = (fake_sources_context_t *)context;
    fake->imu_calls++;
    *sample = fake->imu_sample;
    return fake->imu_result;
}

static int fake_read_env(void *context, nrf_env_sample_t *sample)
{
    fake_sources_context_t *fake = (fake_sources_context_t *)context;
    fake->env_calls++;
    *sample = fake->env_sample;
    return fake->env_result;
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

static fake_sources_context_t make_good_samples(void)
{
    fake_sources_context_t fake;
    memset(&fake, 0, sizeof(fake));

    fake.imu_sample.accel_mg[0] = 10;
    fake.imu_sample.accel_mg[1] = -20;
    fake.imu_sample.accel_mg[2] = 998;
    fake.imu_sample.gyro_mdps[0] = 125;
    fake.imu_sample.gyro_mdps[1] = -250;
    fake.imu_sample.gyro_mdps[2] = 375;
    fake.imu_sample.temperature_milli_c = 24500;

    fake.env_sample.temperature_milli_c = 24625;
    fake.env_sample.pressure_pa = 100845U;
    fake.env_sample.humidity_milli_percent = 48750U;
    return fake;
}

static nrf_sensor_sources_t make_sources(fake_sources_context_t *fake)
{
    const nrf_sensor_sources_t sources = {
        .read_imu = fake_read_imu,
        .imu_context = fake,
        .read_env = fake_read_env,
        .env_context = fake,
    };
    return sources;
}

static void test_remote_refresh_aggregates_both_sensors(void)
{
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    fake_sources_context_t fake = make_good_samples();
    const nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_OK);
    assert(snapshot.sequence == 1U);
    assert(snapshot.valid_mask ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
    assert(snapshot.fault_mask == 0U);
    assert(snapshot.imu.accel_mg[2] == 998);
    assert(snapshot.imu.gyro_mdps[1] == -250);
    assert(snapshot.env.pressure_pa == 100845U);
    assert(snapshot.env.humidity_milli_percent == 48750U);
    assert(fake.imu_calls == 1);
    assert(fake.env_calls == 1);
}

static void test_main_and_unassigned_nodes_never_touch_remote_sensors(void)
{
    fake_sources_context_t fake = make_good_samples();
    const nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    nrf_node_state_t main_state = make_ready_node(NRF_NODE_ROLE_MAIN);
    assert(nrf_sensor_snapshot_refresh(&main_state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_SKIPPED);

    nrf_node_state_t unassigned = make_ready_node(NRF_NODE_ROLE_UNASSIGNED);
    assert(nrf_sensor_snapshot_refresh(&unassigned, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_SKIPPED);

    assert(fake.imu_calls == 0);
    assert(fake.env_calls == 0);
    assert(snapshot.sequence == 0U);
    assert(snapshot.valid_mask == 0U);
    assert(snapshot.fault_mask == 0U);
}

static void test_unavailable_remote_is_not_sampled(void)
{
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    assert(nrf_node_state_mark_unavailable(&state) == NRF_NODE_STATE_OK);

    fake_sources_context_t fake = make_good_samples();
    const nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_ERR_NOT_READY);
    assert(fake.imu_calls == 0);
    assert(fake.env_calls == 0);
    assert(snapshot.sequence == 0U);
}

static void test_partial_failure_keeps_only_fresh_valid_data(void)
{
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    fake_sources_context_t fake = make_good_samples();
    const nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_OK);
    assert(snapshot.imu.accel_mg[2] == 998);

    fake.imu_result = -1;
    fake.env_sample.pressure_pa = 99876U;
    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(snapshot.sequence == 2U);
    assert(snapshot.valid_mask == NRF_SENSOR_VALID_ENV);
    assert(snapshot.fault_mask == NRF_SENSOR_VALID_IMU);
    assert(snapshot.imu.accel_mg[2] == 0);
    assert(snapshot.env.pressure_pa == 99876U);
}

static void test_missing_or_failed_sources_report_no_data(void)
{
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    fake_sources_context_t fake = make_good_samples();
    fake.imu_result = -4;
    fake.env_result = -5;
    nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_ERR_NO_DATA);
    assert(snapshot.valid_mask == 0U);
    assert(snapshot.fault_mask ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));

    sources.read_imu = NULL;
    sources.read_env = NULL;
    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_ERR_NO_DATA);
    assert(snapshot.sequence == 2U);
    assert(snapshot.fault_mask ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
}

static void test_argument_validation(void)
{
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    fake_sources_context_t fake = make_good_samples();
    const nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);

    assert(nrf_sensor_snapshot_refresh(NULL, &sources, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT);
    assert(nrf_sensor_snapshot_refresh(&state, NULL, &snapshot) ==
           NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT);
    assert(nrf_sensor_snapshot_refresh(&state, &sources, NULL) ==
           NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT);
    assert(fake.imu_calls == 0);
    assert(fake.env_calls == 0);
}

static void test_pending_imu_has_no_fault_or_stale_payload(void)
{
    fake_sources_context_t fake = make_good_samples();
    nrf_sensor_sources_t sources = make_sources(&fake);
    nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
    nrf_sensor_snapshot_t snapshot;
    nrf_sensor_snapshot_init(&snapshot);
    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) == NRF_SENSOR_SNAPSHOT_OK);
    fake.imu_result = NRF_SENSOR_READ_PENDING;
    assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) == NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(snapshot.valid_mask == NRF_SENSOR_VALID_ENV && snapshot.fault_mask == 0U);
    assert(snapshot.imu.accel_mg[2] == 0);
}

int main(void)
{
    test_pending_imu_has_no_fault_or_stale_payload();
    test_remote_refresh_aggregates_both_sensors();
    test_main_and_unassigned_nodes_never_touch_remote_sensors();
    test_unavailable_remote_is_not_sampled();
    test_partial_failure_keeps_only_fresh_valid_data();
    test_missing_or_failed_sources_report_no_data();
    test_argument_validation();
    return 0;
}
