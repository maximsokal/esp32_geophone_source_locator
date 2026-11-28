#pragma once

#include "pb2/acquisition/mock_source.hpp"
#include "pb2/acquisition/pru_ring_consumer.hpp"
#include "pb2/power/shutdown_fsm.hpp"
#include "pb2/storage/geoseis_v2_writer.hpp"
#include "pb2/transport/uart_transport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pb2::runtime {

enum class PipelineSourceKind {
    Mock = 0,
    PruSnapshotFile = 1,
    PruLiveFile = 2,
    PruUio = 3,
    PruDevMem = 4,
};

struct PipelineConfig {
    PipelineSourceKind source_kind = PipelineSourceKind::Mock;
    std::uint32_t sample_rate_hz = 20000U;
    pb2::acquisition::AdcWiringProfile profile = pb2::acquisition::AdcWiringProfile::SerialDualDout;
    std::size_t max_frames = 1024U;
    std::string pru_path;
    std::string pru_uio_dev = "/dev/uio0";
    std::uint64_t pru_phys_addr = PB2_AM62X_PRU_SHARED_PHYS;
    std::uint32_t pru_cmd_timeout_ms = 2000U;

    std::string record_base_path;
    bool enable_chunking = false;
    bool allow_overwrite = false;
    std::uint64_t max_chunk_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t max_writer_queue_frames = 8192U;

    bool enable_dsp = true;
    std::size_t preview_capacity = 64U;

    std::string uart_device;
    std::uint32_t uart_baud = 115200U;
    int uart_fd = -1;
    std::uint32_t boot_epoch = 1U;
    std::uint32_t session_epoch = 1U;
};

struct PipelineReport {
    bool ok = false;
    std::uint64_t frames_processed = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t gap_blocks = 0;
    std::uint64_t dsp_triggers = 0;
    std::uint64_t preview_frames_kept = 0;
    std::uint64_t uart_tx_frames = 0;
    std::uint64_t uart_tx_drops = 0;
    std::string recorded_file_path;
    pb2::power::ShutdownState shutdown_state = pb2::power::ShutdownState::Running;
    std::string error_message;
};

class RuntimePipeline {
public:
    explicit RuntimePipeline(const PipelineConfig& cfg);

    PipelineReport run(const std::atomic<bool>& stop_requested);

    const std::deque<pb2::acquisition::SampleFrame>& preview_ring() const noexcept {
        return preview_ring_;
    }

private:
    void push_preview(const pb2::acquisition::SampleFrame* frames, std::size_t count);
    void run_dsp_window(const pb2::acquisition::SampleFrame* frames, std::size_t count, PipelineReport& report);
    void maybe_send_uart_summary(pb2::transport::UartTransport& uart,
                                 const pb2::acquisition::SampleFrame& latest,
                                 PipelineReport& report);

    PipelineConfig cfg_{};
    std::deque<pb2::acquisition::SampleFrame> preview_ring_;
    std::vector<float> dsp_window_;
    std::uint32_t uart_seq_ = 1U;
};

} // namespace pb2::runtime
