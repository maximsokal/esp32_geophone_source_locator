extern "C" {
#include "geophone_events.h"
#include "localization.h"
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define M_CHECK(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Math check failed: %s (%s:%d)\n",         \
                         #cond, __FILE__, __LINE__);                        \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

void test_shared_geophone_event_detectors() {
    std::vector<float> signal(256, 0.5f);
    for (std::size_t i = 0; i < 120; ++i) {
        signal[i] = (i % 2 == 0) ? 0.5f : -0.5f;
    }
    for (std::size_t i = 120; i < 180; ++i) {
        signal[i] = ((i % 2 == 0) ? 25.0f : -25.0f);
    }

    geophone_energy_trigger_config_t ecfg{};
    ecfg.noise_window_samples = 64U;
    ecfg.trigger_window_samples = 16U;
    ecfg.threshold_multiplier = 5.0f;

    geophone_energy_trigger_result_t eres{};
    M_CHECK(geophone_event_energy_trigger_detect(signal.data(), signal.size(), &ecfg, &eres));
    M_CHECK(eres.found);
    M_CHECK(eres.trigger_index >= 105U && eres.trigger_index <= 125U);

    geophone_sta_lta_config_t scfg{};
    scfg.sta_window_samples = 8U;
    scfg.lta_window_samples = 64U;
    scfg.trigger_ratio = 4.0f;

    geophone_sta_lta_result_t sres{};
    M_CHECK(geophone_event_sta_lta_detect(signal.data(), signal.size(), &scfg, &sres));
    M_CHECK(sres.found);
}

void test_shared_tdoa_3d_solver() {
    /* 4 non-coplanar sensors in meters, known source at (1.5, 2.0, 0.8), v = 350 m/s */
    const double speed_mps = 350.0;
    const localization_point3d_t true_src{1.5, 2.0, 0.8};
    const localization_point3d_t sensors[4] = {
        {0.0, 0.0, 0.0},
        {4.0, 0.0, 0.0},
        {0.0, 4.0, 0.0},
        {2.0, 2.0, 2.5},
    };
    M_CHECK(localization_positions_resolve_depth(sensors, 4U));

    localization_observation_t obs[4]{};
    const int64_t t0_us = 1000000LL;
    for (int i = 0; i < 4; ++i) {
        const double dx = true_src.x_m - sensors[i].x_m;
        const double dy = true_src.y_m - sensors[i].y_m;
        const double dz = true_src.z_m - sensors[i].z_m;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        obs[i].sensor_position = sensors[i];
        obs[i].arrival_time_us = t0_us + static_cast<int64_t>(std::llround((dist / speed_mps) * 1e6));
    }

    localization_config_t cfg = localization_default_config();
    cfg.propagation_speed_mps = speed_mps;
    cfg.max_iterations = 40U;
    cfg.convergence_m = 1e-4;
    cfg.damping = 1e-3;

    const localization_point3d_t guess{1.0, 1.0, 1.0};
    localization_result_t res{};
    M_CHECK(localization_tdoa_solve(obs, 4U, &cfg, &guess, &res) == ESP_OK);
    M_CHECK(res.converged);
    M_CHECK(std::fabs(res.source_position.x_m - true_src.x_m) < 0.02);
    M_CHECK(std::fabs(res.source_position.y_m - true_src.y_m) < 0.02);
    M_CHECK(std::fabs(res.source_position.z_m - true_src.z_m) < 0.02);
}

} // namespace

int main() {
    test_shared_geophone_event_detectors();
    test_shared_tdoa_3d_solver();
    std::printf("pb2_shared_math_tests: PASS\n");
    return 0;
}
