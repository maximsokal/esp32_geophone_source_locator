#include "pb2/runtime/pipeline.hpp"
#include "pb2/hardware/pinmap_a0.hpp"

extern "C" {
#include "geophone_events.h"
#include "localization.h"
#include "pb2_protocol.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pb2::runtime {

namespace {

std::uint32_t derive_runtime_epoch(std::uint32_t configured) noexcept {
    if (configured != 0U) {
        return configured;
    }
    const auto now_s = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::uint32_t mixed = static_cast<std::uint32_t>((now_s & 0xFFFFFFFFULL) ^
                                                           static_cast<std::uint64_t>(::getpid()));
    return (mixed < 2U) ? 2U : mixed;
}

bool is_valid_geometry(const GeometryConfig& g) noexcept {
    if (!g.valid || g.propagation_speed_mps <= 0.0 || !std::isfinite(g.propagation_speed_mps)) {
        return false;
    }
    bool seen[4]{false, false, false, false};
    for (std::size_t ch = 0; ch < 4U; ++ch) {
        if (g.channel_to_sensor[ch] >= 4U || seen[g.channel_to_sensor[ch]]) {
            return false;
        }
        seen[g.channel_to_sensor[ch]] = true;
        for (std::size_t k = 0; k < 3U; ++k) {
            if (!std::isfinite(g.sensor_positions_m[ch][k])) {
                return false;
            }
        }
    }
    return true;
}

/// Preallocated circular DDR ring buffer + RAII writer worker thread.
/// Guarantees clean stop + join on all return paths (no std::terminate).
class BoundedRecorderWorker {
public:
    BoundedRecorderWorker(pb2::storage::GeoseisV2Writer& writer,
                          std::size_t capacity_frames,
                          std::uint32_t stall_inject_ms)
        : writer_(writer),
          ring_(capacity_frames > 0U ? capacity_frames : 262144U),
          capacity_(ring_.size()),
          stall_inject_ms_(stall_inject_ms) {}

    ~BoundedRecorderWorker() {
        stop_and_join();
    }

    BoundedRecorderWorker(const BoundedRecorderWorker&) = delete;
    BoundedRecorderWorker& operator=(const BoundedRecorderWorker&) = delete;

    void start() {
        thread_ = std::thread([this]() { run_loop(); });
    }

    bool enqueue(const pb2::acquisition::SampleFrame* frames,
                 std::size_t count,
                 std::uint64_t& out_queued,
                 std::uint64_t& out_dropped) {
        if (count == 0U) {
            return true;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (failed_) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            pb2::acquisition::SampleFrame f = frames[i];
            if (gap_next_) {
                f.flags |= PB2_PRU_BLK_FLAG_GAP_BEFORE;
                gap_next_ = false;
            }
            if (count_ == capacity_) {
                /* Drop oldest frame in preallocated ring & mark gap on next surviving frame */
                head_ = (head_ + 1U) % capacity_;
                --count_;
                ++dropped_;
                ++out_dropped;
                ring_[head_].flags |= PB2_PRU_BLK_FLAG_GAP_BEFORE;
            }
            ring_[tail_] = f;
            tail_ = (tail_ + 1U) % capacity_;
            ++count_;
            ++out_queued;
        }
        cv_.notify_one();
        return true;
    }

    void stop_and_join() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool failed() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return failed_;
    }

private:
    void run_loop() {
        std::vector<pb2::acquisition::SampleFrame> batch;
        batch.reserve(256U);
        for (;;) {
            batch.clear();
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stop_ || count_ > 0U; });
                while (count_ > 0U && batch.size() < 256U) {
                    batch.push_back(ring_[head_]);
                    head_ = (head_ + 1U) % capacity_;
                    --count_;
                }
                if (batch.empty() && stop_) {
                    break;
                }
            }
            if (stall_inject_ms_ > 0U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_inject_ms_));
            }
            if (!batch.empty() && !writer_.write_frames(batch.data(), batch.size())) {
                std::lock_guard<std::mutex> lk(mtx_);
                failed_ = true;
                break;
            }
        }
    }

    pb2::storage::GeoseisV2Writer& writer_;
    std::vector<pb2::acquisition::SampleFrame> ring_;
    std::size_t capacity_ = 0U;
    std::size_t head_ = 0U;
    std::size_t tail_ = 0U;
    std::size_t count_ = 0U;
    std::uint32_t stall_inject_ms_ = 0U;
    bool gap_next_ = false;
    bool stop_ = false;
    bool failed_ = false;
    std::uint64_t dropped_ = 0U;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
};

} // namespace

RuntimePipeline::RuntimePipeline(const PipelineConfig& cfg)
    : cfg_(cfg),
      active_geometry_(cfg.initial_geometry),
      active_boot_epoch_(derive_runtime_epoch(cfg.boot_epoch)),
      active_session_epoch_(derive_runtime_epoch(cfg.session_epoch)) {}

void RuntimePipeline::push_preview(const pb2::acquisition::SampleFrame* frames, std::size_t count) {
    if (cfg_.preview_capacity == 0U || count == 0U) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (preview_ring_.size() >= cfg_.preview_capacity) {
            preview_ring_.pop_front();
        }
        preview_ring_.push_back(frames[i]);
    }
}

std::size_t RuntimePipeline::compute_dsp_window_samples() const noexcept {
    /* noise_window_samples (64) + trigger_window_samples (16) + 16 margin */
    constexpr std::size_t kMinAlgorithmSamples = 96U;
    constexpr std::size_t kDefaultDspWindow = 256U;
    constexpr std::size_t kMaxBoundedWindow = 16384U;

    if (!is_valid_geometry(active_geometry_) || cfg_.sample_rate_hz == 0U) {
        return kDefaultDspWindow;
    }

    double max_baseline_m = 0.0;
    for (std::size_t i = 0; i < 4U; ++i) {
        for (std::size_t j = i + 1U; j < 4U; ++j) {
            const double dx = active_geometry_.sensor_positions_m[i][0] - active_geometry_.sensor_positions_m[j][0];
            const double dy = active_geometry_.sensor_positions_m[i][1] - active_geometry_.sensor_positions_m[j][1];
            const double dz = active_geometry_.sensor_positions_m[i][2] - active_geometry_.sensor_positions_m[j][2];
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > max_baseline_m) {
                max_baseline_m = dist;
            }
        }
    }

    const double max_tdoa_s = max_baseline_m / active_geometry_.propagation_speed_mps;
    const auto tdoa_samples = static_cast<std::size_t>(
        std::ceil(max_tdoa_s * static_cast<double>(cfg_.sample_rate_hz)));
    const std::size_t needed = kMinAlgorithmSamples + tdoa_samples + 32U;
    return std::min(kMaxBoundedWindow, std::max(kDefaultDspWindow, needed));
}

void RuntimePipeline::run_dsp_window(const pb2::acquisition::SampleFrame* frames,
                                     std::size_t count,
                                     pb2::transport::UartTransport& uart,
                                     PipelineReport& report) {
    if (!cfg_.enable_dsp || count == 0U) {
        return;
    }
    const std::size_t max_dsp_window = compute_dsp_window_samples();
    report.dsp_window_samples = max_dsp_window;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t ch = 0; ch < 4U; ++ch) {
            dsp_windows_[ch].push_back(static_cast<float>(frames[i].channels[ch]));
        }
    }
    for (std::size_t ch = 0; ch < 4U; ++ch) {
        if (dsp_windows_[ch].size() > max_dsp_window) {
            const std::size_t excess = dsp_windows_[ch].size() - max_dsp_window;
            dsp_windows_[ch].erase(dsp_windows_[ch].begin(),
                                   dsp_windows_[ch].begin() + static_cast<std::ptrdiff_t>(excess));
        }
    }
    if (dsp_windows_[0].size() >= 96U) {
        geophone_energy_trigger_config_t ecfg{};
        ecfg.noise_window_samples = 64U;
        ecfg.trigger_window_samples = 16U;
        ecfg.threshold_multiplier = 4.5f;

        std::size_t triggered_channels = 0U;
        std::size_t trigger_idx[4]{};
        for (std::size_t ch = 0; ch < 4U; ++ch) {
            geophone_energy_trigger_result_t eres{};
            if (geophone_event_energy_trigger_detect(
                    dsp_windows_[ch].data(), dsp_windows_[ch].size(), &ecfg, &eres) &&
                eres.found) {
                ++triggered_channels;
                trigger_idx[ch] = eres.trigger_index;
            }
        }
        if (triggered_channels > 0U) {
            report.dsp_triggers += triggered_channels;
        }
        /* Never emit synthetic localization without valid session geometry */
        if (triggered_channels == 4U && cfg_.sample_rate_hz > 0U && is_valid_geometry(active_geometry_)) {
            localization_observation_t obs[4]{};
            const double us_per_sample = 1000000.0 / static_cast<double>(cfg_.sample_rate_hz);
            std::int64_t t_sensor[4]{};
            for (std::size_t ch = 0; ch < 4U; ++ch) {
                const std::size_t sidx = active_geometry_.channel_to_sensor[ch];
                obs[sidx].sensor_position = {
                    active_geometry_.sensor_positions_m[sidx][0],
                    active_geometry_.sensor_positions_m[sidx][1],
                    active_geometry_.sensor_positions_m[sidx][2],
                };
                const auto t_us = static_cast<std::int64_t>(
                    std::llround(static_cast<double>(trigger_idx[ch]) * us_per_sample));
                obs[sidx].arrival_time_us = t_us;
                t_sensor[sidx] = t_us;
            }
            localization_config_t lcfg = localization_default_config();
            lcfg.propagation_speed_mps = active_geometry_.propagation_speed_mps;

            localization_point3d_t centroid{0.0, 0.0, 0.0};
            for (std::size_t s = 0; s < 4U; ++s) {
                centroid.x_m += obs[s].sensor_position.x_m * 0.25;
                centroid.y_m += obs[s].sensor_position.y_m * 0.25;
                centroid.z_m += obs[s].sensor_position.z_m * 0.25;
            }
            localization_result_t lres{};
            if (localization_tdoa_solve(obs, 4U, &lcfg, &centroid, &lres) == ESP_OK && lres.converged) {
                report.dsp_localizations++;
                report.last_localization.valid = true;
                report.last_localization.geometry_source = active_geometry_.geometry_source;
                report.last_localization.iterations = lres.iterations;
                report.last_localization.source_x_m = lres.source_position.x_m;
                report.last_localization.source_y_m = lres.source_position.y_m;
                report.last_localization.source_z_m = lres.source_position.z_m;
                report.last_localization.rms_residual_us = lres.rms_residual_us;
                for (std::size_t s = 0; s < 4U; ++s) {
                    report.last_localization.tdoa_us[s] = static_cast<std::int32_t>(t_sensor[s] - t_sensor[0]);
                }

                if (uart.is_open()) {
                    pb2_localization_result_t msg{};
                    msg.session_epoch = active_session_epoch_;
                    msg.valid = 1U;
                    msg.geometry_source = active_geometry_.geometry_source;
                    msg.iterations = lres.iterations;
                    msg.source_x_m = static_cast<float>(lres.source_position.x_m);
                    msg.source_y_m = static_cast<float>(lres.source_position.y_m);
                    msg.source_z_m = static_cast<float>(lres.source_position.z_m);
                    msg.rms_residual_us = static_cast<float>(lres.rms_residual_us);
                    for (std::size_t s = 0; s < 4U; ++s) {
                        msg.tdoa_us[s] = report.last_localization.tdoa_us[s];
                    }
                    std::uint8_t lbuf[40]{};
                    std::uint16_t llen = 0U;
                    if (pb2_encode_localization_result(&msg, lbuf, sizeof(lbuf), &llen) == PB2_PROTO_OK) {
                        pb2_frame_header_t lhdr{PB2_PROTO_VERSION, PB2_MSG_LOCALIZATION_RESULT, 0U,
                                                uart_seq_++, active_boot_epoch_, active_session_epoch_, llen};
                        uart.enqueue_frame(lhdr, lbuf);
                    }
                }
            }
        }
    }
}

void RuntimePipeline::handle_uart_rx_frame(pb2::transport::UartTransport& uart,
                                           const pb2_frame_header_t& rx_hdr,
                                           const uint8_t* rx_payload,
                                           std::atomic<bool>& remote_stop) {
    auto send_ack = [&](std::uint8_t status_code) {
        pb2_ack_payload_t ack{};
        ack.acked_sequence = rx_hdr.sequence;
        ack.acked_msg_type = rx_hdr.msg_type;
        ack.status_code = status_code;
        uint8_t ack_buf[16]{};
        uint16_t ack_len = 0U;
        if (pb2_encode_ack(&ack, ack_buf, sizeof(ack_buf), &ack_len) == PB2_PROTO_OK) {
            pb2_frame_header_t ack_hdr{PB2_PROTO_VERSION, PB2_MSG_ACK, 0U,
                                       uart_seq_++, active_boot_epoch_, active_session_epoch_, ack_len};
            uart.enqueue_frame(ack_hdr, ack_buf);
        }
    };

    if (rx_hdr.msg_type == PB2_MSG_PING) {
        pb2_frame_header_t pong_hdr{};
        pong_hdr.version = PB2_PROTO_VERSION;
        pong_hdr.msg_type = PB2_MSG_PONG;
        pong_hdr.sequence = uart_seq_++;
        pong_hdr.boot_epoch = active_boot_epoch_;
        pong_hdr.session_epoch = active_session_epoch_;
        pong_hdr.payload_len = 0U;
        uart.enqueue_frame(pong_hdr, nullptr);
    } else if (rx_hdr.msg_type == PB2_MSG_HELLO) {
        pb2_capabilities_payload_t cap{0xFFU, cfg_.uart_baud, 2U, 4U};
        std::uint8_t cbuf[32]{};
        std::uint16_t clen = 0U;
        if (pb2_encode_capabilities(&cap, cbuf, sizeof(cbuf), &clen) == PB2_PROTO_OK) {
            pb2_frame_header_t chdr{PB2_PROTO_VERSION, PB2_MSG_CAPABILITIES, 0U,
                                    uart_seq_++, active_boot_epoch_, active_session_epoch_, clen};
            uart.enqueue_frame(chdr, cbuf);
        }
        send_ack(0U);
    } else if (rx_hdr.msg_type == PB2_MSG_GEOMETRY_UPDATE) {
        pb2_geometry_update_t gmsg{};
        if (rx_payload != nullptr &&
            pb2_decode_geometry_update(rx_payload, rx_hdr.payload_len, &gmsg) == PB2_PROTO_OK) {
            GeometryConfig gcfg{};
            gcfg.valid = (gmsg.valid != 0U) && (gmsg.sensor_count == 4U);
            gcfg.geometry_source = gmsg.geometry_source;
            gcfg.session_epoch = (gmsg.session_epoch != 0U) ? gmsg.session_epoch : active_session_epoch_;
            gcfg.propagation_speed_mps = static_cast<double>(gmsg.propagation_speed_mps);
            for (std::size_t ch = 0; ch < 4U; ++ch) {
                gcfg.channel_to_sensor[ch] = gmsg.channel_to_sensor[ch];
                for (std::size_t k = 0; k < 3U; ++k) {
                    gcfg.sensor_positions_m[ch][k] = static_cast<double>(gmsg.sensor_positions_m[ch][k]);
                }
            }
            if (is_valid_geometry(gcfg)) {
                active_geometry_ = gcfg;
                send_ack(0U);
            } else {
                active_geometry_.valid = false;
                send_ack(1U);
            }
        } else {
            send_ack(1U);
        }
    } else if (rx_hdr.msg_type == PB2_MSG_SHUTDOWN_REQUEST) {
        /* Acknowledge receipt immediately; SHUTDOWN_READY is sent ONLY after STOP_PRU->DRAIN->FLUSH->FSYNC->FINALIZE */
        remote_stop.store(true, std::memory_order_release);
        shutdown_via_uart_ = true;
        send_ack(0U);
    }
}

void RuntimePipeline::maybe_send_uart_summary(pb2::transport::UartTransport& uart,
                                              const pb2::acquisition::SampleFrame& latest,
                                              std::atomic<bool>& remote_stop,
                                              PipelineReport& report) {
    if (!uart.is_open()) {
        return;
    }
    pb2_frame_header_t hdr{};
    hdr.version = PB2_PROTO_VERSION;
    hdr.msg_type = PB2_MSG_PREVIEW_BLOCK;
    hdr.flags = 0U;
    hdr.sequence = uart_seq_++;
    hdr.boot_epoch = active_boot_epoch_;
    hdr.session_epoch = active_session_epoch_;

    pb2_preview_block_t msg{};
    msg.first_sample_index = latest.source_sample_index;
    msg.first_timestamp_us = latest.timestamp_us;
    msg.source_sample_rate_hz = cfg_.sample_rate_hz;
    msg.decimation_factor = 1U;
    msg.frame_count = 1U;
    msg.clock_domain = (cfg_.source_kind == PipelineSourceKind::Mock)
                           ? PB2_CLOCK_PB2_LINUX_MONO_US
                           : PB2_CLOCK_PB2_PRU_US;
    msg.sample_format = PB2_SAMPLE_FMT_INT16_ADC;
    msg.channel_count = PB2_PROTO_CHANNEL_COUNT;
    if (cfg_.profile == pb2::acquisition::AdcWiringProfile::SerialDualDout) {
        msg.channel_map[0] = 1U;
        msg.channel_map[1] = 2U;
        msg.channel_map[2] = 5U;
        msg.channel_map[3] = 6U;
    } else {
        for (std::size_t ch = 0; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
            msg.channel_map[ch] = static_cast<uint8_t>(ch + 1U);
        }
    }
    for (std::size_t ch = 0; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        msg.samples_uv[0][ch] = static_cast<float>(latest.channels[ch]);
    }

    uint8_t payload_buf[PB2_PROTO_MAX_PAYLOAD];
    uint16_t payload_len = 0U;
    if (pb2_encode_preview_block(&msg, payload_buf, sizeof(payload_buf), &payload_len) != PB2_PROTO_OK) {
        return;
    }
    hdr.payload_len = payload_len;
    uart.enqueue_frame(hdr, payload_buf);
    uart.poll_once(0, [&](const pb2_frame_header_t& rx_hdr, const uint8_t* rx_payload) {
        handle_uart_rx_frame(uart, rx_hdr, rx_payload, remote_stop);
    });
    report.uart_tx_frames = uart.stats().frames_tx;
    report.uart_rx_frames = uart.stats().frames_rx;
    report.uart_tx_drops = uart.stats().tx_queue_drops;
}

PipelineReport RuntimePipeline::run(const std::atomic<bool>& stop_requested) {
    PipelineReport report{};
    pb2::power::ShutdownFsm shutdown_fsm;
    std::atomic<bool> remote_stop{false};
    shutdown_via_uart_ = false;
    report.active_boot_epoch = active_boot_epoch_;
    report.active_session_epoch = active_session_epoch_;
    report.dsp_window_samples = compute_dsp_window_samples();

    /* Hardware PRU backends (PruUio, PruDevMem) must probe real Linux PRU/IEP clocks.
       Virtual/simulator backends (Mock, PruSnapshotFile, PruLiveFile) use explicit config clocks. */
    if (cfg_.source_kind == PipelineSourceKind::PruUio ||
        cfg_.source_kind == PipelineSourceKind::PruDevMem) {
        const auto clk = pb2::hardware::probe_hardware_pru_iep_clocks(cfg_.clk_debug_root);
        if (!clk.valid) {
            report.error_message = clk.reason;
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }
    } else {
        const auto clk = pb2::hardware::validate_pru_iep_clocks(cfg_.pru_clock_hz, cfg_.iep_clock_hz);
        if (!clk.valid) {
            report.error_message = clk.reason;
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }
    }

    pb2::storage::GeoseisV2Writer writer;
    const bool recording = !cfg_.record_base_path.empty();
    std::unique_ptr<BoundedRecorderWorker> recorder;
    if (recording) {
        pb2::storage::GeoseisWriterConfig wcfg{};
        wcfg.base_path = cfg_.record_base_path;
        wcfg.sample_rate_hz = cfg_.sample_rate_hz;
        wcfg.enable_chunking = cfg_.enable_chunking;
        wcfg.allow_overwrite = cfg_.allow_overwrite;
        wcfg.max_chunk_bytes = cfg_.max_chunk_bytes;
        if (!writer.open(wcfg)) {
            report.error_message = "Failed to open GeoseisV2Writer at " + cfg_.record_base_path;
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }
        report.recorded_file_path = writer.current_path();
        recorder = std::make_unique<BoundedRecorderWorker>(
            writer, cfg_.max_writer_queue_frames, cfg_.writer_stall_inject_ms);
        recorder->start();
    }

    pb2::transport::UartTransport uart(16U);
    if (cfg_.uart_fd >= 0) {
        uart.attach_fd(cfg_.uart_fd, false);
    } else if (!cfg_.uart_device.empty()) {
        uart.open_device(cfg_.uart_device, cfg_.uart_baud);
    }

    std::vector<pb2::acquisition::SampleFrame> batch;
    batch.reserve(64U);

    auto process_batch = [&](const std::vector<pb2::acquisition::SampleFrame>& frames) -> bool {
        if (frames.empty()) {
            return true;
        }
        report.frames_acquired += frames.size();
        if (recorder) {
            std::uint64_t q_drop = 0U;
            if (!recorder->enqueue(frames.data(), frames.size(), report.frames_queued, q_drop)) {
                report.error_message = "GeoseisV2Writer write_frames failed";
                return false;
            }
            if (q_drop > 0U) {
                report.writer_queue_dropped += q_drop;
                report.dropped_frames += q_drop;
                report.degraded = true;
            }
        }
        push_preview(frames.data(), frames.size());
        run_dsp_window(frames.data(), frames.size(), uart, report);
        maybe_send_uart_summary(uart, frames.back(), remote_stop, report);
        report.frames_processed += frames.size();
        return true;
    };

    if (cfg_.source_kind == PipelineSourceKind::Mock) {
        pb2::acquisition::MockSourceConfig mcfg{};
        mcfg.sample_rate_hz = cfg_.sample_rate_hz;
        mcfg.profile = cfg_.profile;
        pb2::acquisition::MockAcquisitionSource source(mcfg);
        if (!source.is_valid()) {
            report.error_message = "Invalid MockSourceConfig";
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }

        while (!stop_requested.load(std::memory_order_acquire) &&
               !remote_stop.load(std::memory_order_acquire) &&
               (cfg_.max_frames == 0U || report.frames_processed < cfg_.max_frames)) {
            const std::size_t want = (cfg_.max_frames == 0U)
                                         ? 64U
                                         : std::min<std::size_t>(64U, cfg_.max_frames - report.frames_processed);
            batch.clear();
            if (!source.read_frames(want, batch) || batch.empty()) {
                break;
            }
            if (!process_batch(batch)) {
                break;
            }
        }
        report.dropped_frames += source.stats().dropped_frames;
    } else {
        pb2::acquisition::PruSharedMemory shm;
        bool opened = false;
        const bool is_live = (cfg_.source_kind != PipelineSourceKind::PruSnapshotFile);
        if (cfg_.source_kind == PipelineSourceKind::PruSnapshotFile) {
            opened = shm.open_file(cfg_.pru_path, false);
        } else if (cfg_.source_kind == PipelineSourceKind::PruLiveFile) {
            opened = shm.open_file(cfg_.pru_path, true);
        } else if (cfg_.source_kind == PipelineSourceKind::PruUio) {
            opened = shm.open_uio(cfg_.pru_uio_dev, cfg_.pru_phys_addr);
        } else {
            opened = shm.open_dev_mem(cfg_.pru_phys_addr, true);
        }
        if (!opened) {
            report.error_message = "Failed to open PRU shared memory backend";
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }

        pb2::acquisition::PruRingConsumer consumer(shm.get());
        if (is_live) {
            const auto wait_end = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(cfg_.pru_cmd_timeout_ms);
            while (!consumer.is_valid() && std::chrono::steady_clock::now() < wait_end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!consumer.is_valid()) {
            report.error_message = "Invalid PRU shared memory header (magic/version/channels)";
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            report.shutdown_state = shutdown_fsm.state();
            return report;
        }

        if (shm.get()->session_epoch != 0U) {
            active_session_epoch_ = shm.get()->session_epoch;
            report.active_session_epoch = active_session_epoch_;
        }

        if (is_live) {
            if (!consumer.start_acquisition(cfg_.sample_rate_hz,
                                            cfg_.profile,
                                            cfg_.iep_clock_hz,
                                            active_session_epoch_,
                                            cfg_.pru_cmd_timeout_ms)) {
                report.error_message = "PRU lifecycle CONFIGURE->ARM->START failed";
                shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
                report.shutdown_state = shutdown_fsm.state();
                return report;
            }
        }

        while (!stop_requested.load(std::memory_order_acquire) &&
               !remote_stop.load(std::memory_order_acquire)) {
            batch.clear();
            const std::size_t got = consumer.drain(16U, batch);
            if (got > 0U) {
                if (!process_batch(batch)) {
                    break;
                }
                if (cfg_.max_frames > 0U && report.frames_processed >= cfg_.max_frames) {
                    break;
                }
            } else {
                if (!is_live) {
                    break;
                }
                if (consumer.state() == PB2_PRU_STATE_FAULT) {
                    report.error_message = "PRU entered FAULT state during acquisition";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        shutdown_fsm.on_event(pb2::power::ShutdownEvent::RequestStop);
        if (is_live) {
            if (!consumer.stop_acquisition(cfg_.pru_cmd_timeout_ms) && report.error_message.empty()) {
                report.error_message = "PRU STOP->DRAIN->STOPPED timed out";
            }
            batch.clear();
            if (consumer.drain(0U, batch) > 0U) {
                process_batch(batch);
            }
        }
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped);

        const auto pstats = consumer.snapshot_stats();
        report.pru_scheduled_samples = shm.get()->total_frames + pstats.missed_deadlines;
        report.pru_missed_deadlines = pstats.missed_deadlines;
        report.pru_adc_dropped = (pstats.hw_error_flags & PB2_PRU_ERR_BUSY_FELL_IN_READ) ? 1U : 0U;
        report.pru_ring_overflow_dropped = pstats.dropped_frames;
        report.dropped_frames += pstats.dropped_frames;
        report.gap_blocks = pstats.gap_blocks;
        if (pstats.dropped_frames > 0U || pstats.missed_deadlines > 0U || pstats.overrun_count > 0U) {
            report.degraded = true;
        }
    }

    if (shutdown_fsm.state() == pb2::power::ShutdownState::Running) {
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::RequestStop);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped);
    }

    if (recorder) {
        recorder->stop_and_join();
        if (recorder->failed() && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer write_frames failed";
        }
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::StorageFlushed);
        if (!writer.flush() && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer flush/fsync failed";
        }
        report.frames_written = writer.frames_written();
        writer.close();
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::SessionFinalized);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::PruStopped);

        /* Verify frame balance without double counting */
        if (report.frames_acquired != (report.frames_written + report.writer_queue_dropped) &&
            report.error_message.empty()) {
            report.error_message = "Frame accounting mismatch (unannounced frame loss)";
        }
    } else {
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::StorageFlushed);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::SessionFinalized);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::PruStopped);
    }

    if (!report.error_message.empty()) {
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
    } else if (shutdown_via_uart_ && uart.is_open() && !report.degraded) {
        /* Only send SHUTDOWN_READY after STOP_PRU->DRAIN->FLUSH->FSYNC->FINALIZE succeeded */
        pb2_frame_header_t rdy{PB2_PROTO_VERSION, PB2_MSG_SHUTDOWN_READY, 0U,
                               uart_seq_++, active_boot_epoch_, active_session_epoch_, 0U};
        if (!uart.enqueue_frame(rdy, nullptr)) {
            report.error_message = "UART TX queue rejected SHUTDOWN_READY";
            shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
        } else {
            const auto tx_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (uart.pending_tx_frames() > 0U && std::chrono::steady_clock::now() < tx_deadline) {
                (void)uart.poll_once(10, [&](const pb2_frame_header_t&, const uint8_t*) {});
            }
            if (uart.pending_tx_frames() > 0U) {
                report.error_message = "UART TX failed to flush SHUTDOWN_READY";
                shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
            } else {
                report.shutdown_ready_sent = true;
            }
        }
    }

    if (uart.is_open()) {
        report.uart_tx_frames = uart.stats().frames_tx;
        report.uart_rx_frames = uart.stats().frames_rx;
        report.uart_tx_drops = uart.stats().tx_queue_drops;
        uart.close_device();
    }

    report.preview_frames_kept = preview_ring_.size();
    report.shutdown_state = shutdown_fsm.state();
    report.ok = report.error_message.empty() && !report.degraded;
    return report;
}

} // namespace pb2::runtime
