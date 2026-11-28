#pragma once

#include "pb2/acquisition/mock_source.hpp"
#include "pb2/acquisition/pru_ring_consumer.hpp"
#include "pb2/power/shutdown_fsm.hpp"
#include "pb2/storage/geoseis_v2_writer.hpp"
#include "pb2/transport/uart_transport.hpp"

#include <array>
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

struct GeometryConfig {
    bool valid = false;
    std::uint8_t geometry_source = PB2_GEOMETRY_SRC_NONE;
    std::uint32_t session_epoch = 0U;
    double propagation_speed_mps = 0.0;
    std::array<std::array<double, 3>, 4> sensor_positions_m{};
    std::array<std::uint8_t, 4> channel_to_sensor{0U, 1U, 2U, 3U};
};

struct LocalizationSummary {
    bool valid = false;
    std::uint8_t geometry_source = PB2_GEOMETRY_SRC_NONE;
    std::uint8_t iterations = 0U;
    double source_x_m = 0.0;
    double source_y_m = 0.0;
    double source_z_m = 0.0;
    double rms_residual_us = 0.0;
    std::array<std::int32_t, 4> tdoa_us{};
};

struct PipelineConfig {
    PipelineSourceKind source_kind = PipelineSourceKind::Mock;
    std::uint32_t sample_rate_hz = 20000U;
    pb2::acquisition::AdcWiringProfile profile = pb2::acquisition::AdcWiringProfile::SerialDualDout;
    std::size_t max_frames = 1024U;
    std::string pru_path;
    std::string pru_uio_dev;
    std::uint64_t pru_phys_addr = PB2_AM62X_PRU_SHARED_PHYS;
    std::uint32_t pru_clock_hz = 200000000U;
    std::uint32_t iep_clock_hz = 200000000U;
    std::string clk_debug_root = "/sys/kernel/debug/clk";
    std::uint32_t pru_cmd_timeout_ms = 2000U;

    std::string record_base_path;
    bool enable_chunking = false;
    bool allow_overwrite = false;
    std::uint64_t max_chunk_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    /* 262144 SampleFrames (~1.31 s @ 200 kHz, ~10 MB preallocated ring in 512 MB LPDDR4 A1) */
    std::size_t max_writer_queue_frames = 262144U;
    std::uint32_t writer_stall_inject_ms = 0U; /* For deterministic virtual SD stall tests */

    bool enable_dsp = true;
    GeometryConfig initial_geometry{};
    std::size_t preview_capacity = 64U;

    std::string uart_device;
    std::uint32_t uart_baud = 115200U;
    int uart_fd = -1;
    std::uint32_t boot_epoch = 0U;
    std::uint32_t session_epoch = 0U;
};

struct PipelineReport {
    bool ok = false;
    bool degraded = false;
    bool shutdown_ready_sent = false;
    std::uint32_t active_boot_epoch = 0;
    std::uint32_t active_session_epoch = 0;
    std::uint64_t frames_processed = 0;
    std::uint64_t frames_acquired = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t pru_scheduled_samples = 0;
    std::uint64_t pru_missed_deadlines = 0;
    std::uint64_t pru_adc_dropped = 0;
    std::uint64_t pru_ring_overflow_dropped = 0;
    std::uint64_t writer_queue_dropped = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t gap_blocks = 0;
    std::uint64_t dsp_triggers = 0;
    std::uint64_t dsp_localizations = 0;
    std::size_t dsp_window_samples = 0;
    LocalizationSummary last_localization{};
    std::uint64_t preview_frames_kept = 0;
    std::uint64_t uart_tx_frames = 0;
    std::uint64_t uart_rx_frames = 0;
    std::uint64_t uart_tx_drops = 0;
    std::string recorded_file_path;
    pb2::power::ShutdownState shutdown_state = pb2::power::ShutdownState::Running;
    std::string error_message;
};

class RuntimePipeline {
public:
    explicit RuntimePipeline(const PipelineConfig& cfg);

    PipelineReport run(const std::atomic<bool>& stop_requested);

    /// Production UART RX handler (HELLO/PING/HEARTBEAT/GEOMETRY_UPDATE/SHUTDOWN_REQUEST).
    void handle_uart_rx_frame(pb2::transport::UartTransport& uart,
                              const pb2_frame_header_t& rx_hdr,
                              const uint8_t* rx_payload,
                              std::atomic<bool>& remote_stop);

    const GeometryConfig& active_geometry() const noexcept { return active_geometry_; }

    const std::deque<pb2::acquisition::SampleFrame>& preview_ring() const noexcept {
        return preview_ring_;
    }

private:
    void push_preview(const pb2::acquisition::SampleFrame* frames, std::size_t count);
    std::size_t compute_dsp_window_samples() const noexcept;
    void run_dsp_window(const pb2::acquisition::SampleFrame* frames,
                        std::size_t count,
                        pb2::transport::UartTransport& uart,
                        PipelineReport& report);
    void maybe_send_uart_summary(pb2::transport::UartTransport& uart,
                                 const pb2::acquisition::SampleFrame& latest,
                                 std::atomic<bool>& remote_stop,
                                 PipelineReport& report);

    PipelineConfig cfg_{};
    GeometryConfig active_geometry_{};
    std::uint32_t active_boot_epoch_ = 0U;
    std::uint32_t active_session_epoch_ = 0U;
    bool shutdown_via_uart_ = false;
    std::deque<pb2::acquisition::SampleFrame> preview_ring_;
    std::array<std::vector<float>, 4> dsp_windows_;
    std::uint32_t uart_seq_ = 1U;
};

} // namespace pb2::runtime
