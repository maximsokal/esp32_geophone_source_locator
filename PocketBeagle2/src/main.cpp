#include "pb2/acquisition/mock_source.hpp"
#include "pb2/diagnostics/runtime_info.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout << "Usage: pb2_node [--doctor-info] [--mock-smoke] [--frames N] [--rate HZ]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    bool doctor_info = false;
    bool mock_smoke = false;
    std::size_t frames = 16;
    std::uint32_t rate_hz = 500;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--doctor-info") {
            doctor_info = true;
        } else if (arg == "--mock-smoke") {
            mock_smoke = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--rate" && i + 1 < argc) {
            rate_hz = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (!doctor_info && !mock_smoke) {
        doctor_info = true;
        mock_smoke = true;
    }

    if (doctor_info) {
        const auto info = pb2::diagnostics::collect_runtime_info();
        std::cout << "pb2_node version=" << info.app_version
                  << " arch=" << info.arch
                  << " os=" << info.os_name
                  << " target=\"" << info.board_target << "\"\n";
    }

    if (mock_smoke) {
        pb2::acquisition::MockSourceConfig cfg{};
        cfg.sample_rate_hz = rate_hz;
        cfg.profile = pb2::acquisition::AdcWiringProfile::SerialDualDout;

        pb2::acquisition::MockAcquisitionSource source(cfg);
        std::vector<pb2::acquisition::SampleFrame> batch;
        if (!source.read_frames(frames, batch)) {
            std::cerr << "MockAcquisitionSource failed for rate=" << rate_hz << "\n";
            return 1;
        }

        const auto map = source.physical_channel_map();
        const auto& stats = source.stats();
        std::cout << "mock_smoke rate_hz=" << rate_hz
                  << " frames=" << stats.generated_frames
                  << " dropped=" << stats.dropped_frames
                  << " last_idx=" << stats.last_sample_index
                  << " last_ts_us=" << stats.last_timestamp_us
                  << " adc_map=[" << static_cast<int>(map[0]) << ","
                  << static_cast<int>(map[1]) << ","
                  << static_cast<int>(map[2]) << ","
                  << static_cast<int>(map[3]) << "]\n";
    }

    return 0;
}
