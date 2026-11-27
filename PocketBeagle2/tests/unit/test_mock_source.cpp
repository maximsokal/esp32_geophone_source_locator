#include "pb2/acquisition/mock_source.hpp"
#include "pb2/diagnostics/runtime_info.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define PB2_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Check failed: %s (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                               \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

void test_invalid_rates_rejected() {
    pb2::acquisition::MockSourceConfig zero_cfg{};
    zero_cfg.sample_rate_hz = 0U;
    pb2::acquisition::MockAcquisitionSource zero_src(zero_cfg);
    PB2_CHECK(!zero_src.is_valid());
    std::vector<pb2::acquisition::SampleFrame> out;
    PB2_CHECK(!zero_src.read_frames(4, out));
    PB2_CHECK(out.empty());

    pb2::acquisition::MockSourceConfig over_cfg{};
    over_cfg.sample_rate_hz = 200001U;
    pb2::acquisition::MockAcquisitionSource over_src(over_cfg);
    PB2_CHECK(!over_src.is_valid());
    PB2_CHECK(!over_src.read_frames(4, out));
}

void test_channel_mapping_profiles() {
    pb2::acquisition::MockSourceConfig single_cfg{};
    single_cfg.profile = pb2::acquisition::AdcWiringProfile::SerialSingleDout;
    pb2::acquisition::MockAcquisitionSource single_src(single_cfg);
    const auto single_map = single_src.physical_channel_map();
    PB2_CHECK((single_map == std::array<std::uint8_t, 4>{1U, 2U, 3U, 4U}));

    pb2::acquisition::MockSourceConfig dual_cfg{};
    dual_cfg.profile = pb2::acquisition::AdcWiringProfile::SerialDualDout;
    pb2::acquisition::MockAcquisitionSource dual_src(dual_cfg);
    const auto dual_map = dual_src.physical_channel_map();
    PB2_CHECK((dual_map == std::array<std::uint8_t, 4>{1U, 2U, 5U, 6U}));
}

void test_200k_timestamps_and_indices() {
    pb2::acquisition::MockSourceConfig cfg{};
    cfg.sample_rate_hz = 200000U;
    cfg.start_sample_index = 100ULL;
    cfg.start_timestamp_us = 1000ULL;

    pb2::acquisition::MockAcquisitionSource src(cfg);
    PB2_CHECK(src.is_valid());

    std::vector<pb2::acquisition::SampleFrame> out;
    PB2_CHECK(src.read_frames(4, out));
    PB2_CHECK(out.size() == 4U);
    PB2_CHECK(out[0].source_sample_index == 100ULL);
    PB2_CHECK(out[0].timestamp_us == 1000ULL);
    PB2_CHECK(out[1].source_sample_index == 101ULL);
    PB2_CHECK(out[1].timestamp_us == 1005ULL);
    PB2_CHECK(out[3].source_sample_index == 103ULL);
    PB2_CHECK(out[3].timestamp_us == 1015ULL);
}

void test_explicit_drop_accounting() {
    pb2::acquisition::MockSourceConfig cfg{};
    cfg.sample_rate_hz = 1000U;
    cfg.inject_drop_every_n_frames = 3U;

    pb2::acquisition::MockAcquisitionSource src(cfg);
    std::vector<pb2::acquisition::SampleFrame> out;
    PB2_CHECK(src.read_frames(4, out));
    PB2_CHECK(out.size() == 4U);
    PB2_CHECK(src.stats().generated_frames == 4ULL);
    PB2_CHECK(src.stats().dropped_frames == 1ULL);
    PB2_CHECK(out[0].source_sample_index == 0ULL);
    PB2_CHECK(out[1].source_sample_index == 1ULL);
    PB2_CHECK(out[2].source_sample_index == 3ULL); // index 2 dropped explicitly
    PB2_CHECK(out[3].source_sample_index == 4ULL);

    PB2_CHECK(src.read_frames(1, out));
    PB2_CHECK(src.stats().dropped_frames == 2ULL);
    PB2_CHECK(out[0].source_sample_index == 6ULL); // index 5 dropped explicitly
}

void test_runtime_info() {
    const auto info = pb2::diagnostics::collect_runtime_info();
    PB2_CHECK(!info.app_version.empty());
    PB2_CHECK(!info.arch.empty());
    PB2_CHECK(!info.board_target.empty());
}

} // namespace

int main() {
    test_invalid_rates_rejected();
    test_channel_mapping_profiles();
    test_200k_timestamps_and_indices();
    test_explicit_drop_accounting();
    test_runtime_info();
    return 0;
}
