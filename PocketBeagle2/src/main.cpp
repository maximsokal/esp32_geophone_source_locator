#include "pb2/acquisition/mock_source.hpp"
#include "pb2/diagnostics/runtime_info.hpp"
#include "pb2/hardware/pinmap_a0.hpp"
#include "pb2/runtime/pipeline.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int /*sig*/) {
    g_stop_requested.store(true, std::memory_order_release);
}

void print_usage() {
    std::cout << "Usage: pb2_node [--mode mock|diagnostics|pru-diag|acquire|record|playback|uart-endpoint]\n"
              << "                [--doctor-info] [--mock-smoke] [--frames N] [--rate HZ]\n"
              << "                [--pru-file PATH] [--pru-devmem HEX_ADDR] [--out BASE_PATH]\n"
              << "                [--uart DEV] [--baud RATE] [--no-dsp]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string mode;
    bool doctor_info = false;
    bool mock_smoke = false;
    std::size_t frames = 16U;
    std::uint32_t rate_hz = 500U;
    std::string pru_file;
    std::uint64_t pru_devmem = 0x30010000ULL;
    std::string out_path;
    std::string uart_dev;
    std::uint32_t baud = 115200U;
    bool enable_dsp = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--doctor-info") {
            doctor_info = true;
        } else if (arg == "--mock-smoke") {
            mock_smoke = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--rate" && i + 1 < argc) {
            rate_hz = static_cast<std::uint32_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--pru-file" && i + 1 < argc) {
            pru_file = argv[++i];
        } else if (arg == "--pru-devmem" && i + 1 < argc) {
            pru_devmem = std::strtoull(argv[++i], nullptr, 16);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--uart" && i + 1 < argc) {
            uart_dev = argv[++i];
        } else if (arg == "--baud" && i + 1 < argc) {
            baud = static_cast<std::uint32_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--no-dsp") {
            enable_dsp = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (mode.empty() && !doctor_info && !mock_smoke) {
        doctor_info = true;
        mock_smoke = true;
    }

    if (doctor_info || mode == "diagnostics") {
        const auto info = pb2::diagnostics::collect_runtime_info();
        std::cout << "pb2_node version=" << info.app_version
                  << " arch=" << info.arch
                  << " os=" << info.os_name
                  << " target=\"" << info.board_target << "\""
                  << " pinmap_ok=" << (pb2::hardware::verify_a0_pinmap_conflict_free() ? 1 : 0)
                  << "\n";
        if (mode == "diagnostics" && !mock_smoke) {
            return 0;
        }
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
        if (mode.empty()) {
            return 0;
        }
    }

    if (!mode.empty()) {
        pb2::runtime::PipelineConfig pcfg{};
        pcfg.sample_rate_hz = rate_hz;
        pcfg.max_frames = frames;
        pcfg.record_base_path = out_path;
        pcfg.uart_device = uart_dev;
        pcfg.uart_baud = baud;
        pcfg.enable_dsp = enable_dsp;

        if (!pru_file.empty() || mode == "playback" || mode == "pru-diag") {
            pcfg.source_kind = !pru_file.empty()
                                   ? pb2::runtime::PipelineSourceKind::PruFile
                                   : pb2::runtime::PipelineSourceKind::PruDevMem;
            pcfg.pru_path = pru_file;
            pcfg.pru_phys_addr = pru_devmem;
        } else {
            pcfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
        }

        pb2::runtime::RuntimePipeline pipeline(pcfg);
        const auto report = pipeline.run(g_stop_requested);
        if (!report.ok) {
            std::cerr << "pipeline error: " << report.error_message << "\n";
            return 1;
        }
        std::cout << "pipeline mode=" << mode
                  << " frames=" << report.frames_processed
                  << " dropped=" << report.dropped_frames
                  << " gaps=" << report.gap_blocks
                  << " dsp_triggers=" << report.dsp_triggers
                  << " preview=" << report.preview_frames_kept
                  << " uart_tx=" << report.uart_tx_frames
                  << " file=\"" << report.recorded_file_path << "\"\n";
    }

    return 0;
}
