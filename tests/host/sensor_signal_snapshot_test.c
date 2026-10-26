#define main snapshot_baseline_main
#include "nrf_sensor_snapshot_test.c"
#undef main

#include <stdio.h>

static void test_source_result_cross_product(void)
{
    for (int imu = -9; imu <= 5; ++imu) {
        for (int env = -9; env <= 5; ++env) {
            for (unsigned missing = 0U; missing < 4U; ++missing) {
                nrf_node_state_t state = make_ready_node(NRF_NODE_ROLE_REMOTE);
                fake_sources_context_t fake = make_good_samples();
                fake.imu_result = imu;
                fake.env_result = env;
                nrf_sensor_sources_t sources = make_sources(&fake);
                if ((missing & 1U) != 0U) sources.read_imu = NULL;
                if ((missing & 2U) != 0U) sources.read_env = NULL;
                nrf_sensor_snapshot_t snapshot;
                memset(&snapshot, 0x55, sizeof(snapshot));
                snapshot.sequence = UINT32_MAX;
                const unsigned valid = ((imu == 0 && (missing & 1U) == 0U) ? 1U : 0U) |
                    ((env == 0 && (missing & 2U) == 0U) ? 2U : 0U);
                const int result = nrf_sensor_snapshot_refresh(&state, &sources, &snapshot);
                assert(result == (valid == 3U ? NRF_SENSOR_SNAPSHOT_OK :
                    valid != 0U ? NRF_SENSOR_SNAPSHOT_PARTIAL : NRF_SENSOR_SNAPSHOT_ERR_NO_DATA));
                assert(snapshot.valid_mask == valid && snapshot.fault_mask == (3U ^ valid));
                assert(snapshot.sequence == 0U);
                assert(fake.imu_calls == ((missing & 1U) == 0U ? 1 : 0));
                assert(fake.env_calls == ((missing & 2U) == 0U ? 1 : 0));
                if ((valid & 1U) == 0U) {
                    const nrf_imu_sample_t zero = {0};
                    assert(memcmp(&snapshot.imu, &zero, sizeof(zero)) == 0);
                } else assert(snapshot.imu.accel_mg[2] == fake.imu_sample.accel_mg[2]);
                if ((valid & 2U) == 0U) {
                    const nrf_env_sample_t zero = {0};
                    assert(memcmp(&snapshot.env, &zero, sizeof(zero)) == 0);
                } else assert(snapshot.env.pressure_pa == fake.env_sample.pressure_pa);
            }
        }
    }
    puts("SIGNAL_CASE SNAPSHOT.SOURCES PASS combinations=900");
}

static void test_role_and_lifecycle_domains(void)
{
    for (unsigned role = 0U; role <= UINT8_MAX; ++role) {
        for (unsigned lifecycle = 0U; lifecycle <= UINT8_MAX; ++lifecycle) {
            nrf_node_state_t state = {(nrf_node_role_t)role, (nrf_node_lifecycle_t)lifecycle};
            fake_sources_context_t fake = make_good_samples();
            const nrf_sensor_sources_t sources = make_sources(&fake);
            nrf_sensor_snapshot_t snapshot;
            nrf_sensor_snapshot_init(&snapshot);
            const int expected = lifecycle != NRF_NODE_LIFECYCLE_READY ? NRF_SENSOR_SNAPSHOT_ERR_NOT_READY :
                role != NRF_NODE_ROLE_REMOTE ? NRF_SENSOR_SNAPSHOT_SKIPPED : NRF_SENSOR_SNAPSHOT_OK;
            assert(nrf_sensor_snapshot_refresh(&state, &sources, &snapshot) == expected);
            assert(fake.imu_calls == (expected == NRF_SENSOR_SNAPSHOT_OK ? 1 : 0));
            assert(fake.env_calls == fake.imu_calls);
        }
    }
    puts("SIGNAL_CASE SNAPSHOT.ROLE_LIFECYCLE PASS combinations=65536");
}

int main(void)
{
    assert(snapshot_baseline_main() == 0);
    test_source_result_cross_product();
    test_role_and_lifecycle_domains();
    return 0;
}
