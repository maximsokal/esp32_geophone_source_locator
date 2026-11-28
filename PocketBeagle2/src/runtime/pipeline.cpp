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
#include <vector>

namespace pb2::runtime {

namespace {

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
    : cfg_(cfg) {}

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

void RuntimePipeline::run_dsp_window(const pb2::acquisition::SampleFrame* frames,
                                     std::size_t count,
                                     PipelineReport& report) {
    if (!cfg_.enable_dsp || count == 0U) {
        return;
    }
    constexpr std::size_t kMaxDspWindow = 256U;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t ch = 0; ch < 4U; ++ch) {
            dsp_windows_[ch].push_back(static_cast<float>(frames[i].channels[ch]));
        }
    }
    for (std::size_t ch = 0; ch < 4U; ++ch) {
        if (dsp_windows_[ch].size() > kMaxDspWindow) {
            const std::size_t excess = dsp_windows_[ch].size() - kMaxDspWindow;
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
        if (triggered_channels == 4U && cfg_.sample_rate_hz > 0U) {
            const localization_point3d_t sensors[4] = {
                {0.0, 0.0, 0.0},
                {4.0, 0.0, 0.0},
                {0.0, 4.0, 0.0},
                {2.0, 2.0, 2.5},
            };
            localization_observation_t obs[4]{};
            const double us_per_sample = 1000000.0 / static_cast<double>(cfg_.sample_rate_hz);
            for (std::size_t ch = 0; ch < 4U; ++ch) {
                obs[ch].sensor_position = sensors[ch];
                obs[ch].arrival_time_us = static_cast<int64_t>(
                    std::llround(static_cast<double>(trigger_idx[ch]) * us_per_sample));
            }
            localization_config_t lcfg = localization_default_config();
            const localization_point3d_t guess{1.0, 1.0, 1.0};
            localization_result_t lres{};
            if (localization_tdoa_solve(obs, 4U, &lcfg, &guess, &lres) == ESP_OK && lres.converged) {
                report.dsp_localizations++;
            }
        }
    }
}

void RuntimePipeline::handle_uart_rx_frame(pb2::transport::UartTransport& uart,
                                           const pb2_frame_header_t& rx_hdr,
                                           const uint8_t* /*rx_payload*/,
                                           std::atomic<bool>& remote_stop) {
    if (rx_hdr.msg_type == PB2_MSG_PING) {
        pb2_frame_header_t pong_hdr{};
        pong_hdr.version = PB2_PROTO_VERSION;
        pong_hdr.msg_type = PB2_MSG_PONG;
        pong_hdr.sequence = uart_seq_++;
        pong_hdr.boot_epoch = cfg_.boot_epoch;
        pong_hdr.session_epoch = cfg_.session_epoch;
        pong_hdr.payload_len = 0U;
        uart.enqueue_frame(pong_hdr, nullptr);
    } else if (rx_hdr.msg_type == PB2_MSG_HELLO) {
        pb2_capabilities_payload_t cap{0xFFU, cfg_.uart_baud, 2U, 4U};
        std::uint8_t cbuf[32]{};
        std::uint16_t clen = 0U;
        if (pb2_encode_capabilities(&cap, cbuf, sizeof(cbuf), &clen) == PB2_PROTO_OK) {
            pb2_frame_header_t chdr{PB2_PROTO_VERSION, PB2_MSG_CAPABILITIES, 0U,
                                    uart_seq_++, cfg_.boot_epoch, cfg_.session_epoch, clen};
            uart.enqueue_frame(chdr, cbuf);
        }
        pb2_ack_payload_t ack{};
        ack.acked_sequence = rx_hdr.sequence;
        ack.acked_msg_type = rx_hdr.msg_type;
        ack.status_code = 0U;
        uint8_t ack_buf[16]{};
        uint16_t ack_len = 0U;
        if (pb2_encode_ack(&ack, ack_buf, sizeof(ack_buf), &ack_len) == PB2_PROTO_OK) {
            pb2_frame_header_t ack_hdr{PB2_PROTO_VERSION, PB2_MSG_ACK, 0U,
                                       uart_seq_++, cfg_.boot_epoch, cfg_.session_epoch, ack_len};
            uart.enqueue_frame(ack_hdr, ack_buf);
        }
    } else if (rx_hdr.msg_type == PB2_MSG_SHUTDOWN_REQUEST) {
        remote_stop.store(true, std::memory_order_release);
        pb2_frame_header_t rdy{PB2_PROTO_VERSION, PB2_MSG_SHUTDOWN_READY, 0U,
                               uart_seq_++, cfg_.boot_epoch, cfg_.session_epoch, 0U};
        uart.enqueue_frame(rdy, nullptr);
    }
}

void RuntimePipeline::maybe_send_uart_summary(pb2::transport::UartTransport& uart,
                                              const pb2::acquisition::SampleFrame& latest,
                                              std::uint32_t active_session_epoch,
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
    hdr.boot_epoch = cfg_.boot_epoch;
    hdr.session_epoch = active_session_epoch;

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
    std::uint32_t active_session_epoch = cfg_.session_epoch;

    /* Validate PRU/IEP clocks before hardware PRU acquisition */
    if (cfg_.source_kind == PipelineSourceKind::PruUio ||
        cfg_.source_kind == PipelineSourceKind::PruDevMem) {
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
                report.dropped_frames += q_drop;
                report.degraded = true;
            }
        }
        push_preview(frames.data(), frames.size());
        run_dsp_window(frames.data(), frames.size(), report);
        maybe_send_uart_summary(uart, frames.back(), active_session_epoch, remote_stop, report);
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
            active_session_epoch = shm.get()->session_epoch;
        }

        if (is_live) {
            if (!consumer.start_acquisition(cfg_.sample_rate_hz,
                                            cfg_.profile,
                                            cfg_.iep_clock_hz,
                                            active_session_epoch,
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

        if (is_live) {
            if (!consumer.stop_acquisition(cfg_.pru_cmd_timeout_ms) && report.error_message.empty()) {
                report.error_message = "PRU STOP->DRAIN->STOPPED timed out";
            }
            batch.clear();
            if (consumer.drain(0U, batch) > 0U) {
                process_batch(batch);
            }
        }

        const auto pstats = consumer.snapshot_stats();
        report.dropped_frames += pstats.dropped_frames;
        report.gap_blocks = pstats.gap_blocks;
    }

    if (recorder) {
        recorder->stop_and_join();
        if (recorder->failed() && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer write_frames failed";
        }
        if (!writer.flush() && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer flush/fsync failed";
        }
        report.frames_written = writer.frames_written();
        writer.close();
    }

    if (report.error_message.empty() && !report.degraded) {
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::RequestStop);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::StorageFlushed);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::SessionFinalized);
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::PruStopped);
    } else {
        shutdown_fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError);
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
