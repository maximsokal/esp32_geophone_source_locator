#include "pb2/runtime/pipeline.hpp"

extern "C" {
#include "geophone_events.h"
#include "pb2_protocol.h"
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace pb2::runtime {

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
        dsp_window_.push_back(static_cast<float>(frames[i].channels[0]));
    }
    if (dsp_window_.size() > kMaxDspWindow) {
        const std::size_t excess = dsp_window_.size() - kMaxDspWindow;
        dsp_window_.erase(dsp_window_.begin(), dsp_window_.begin() + static_cast<std::ptrdiff_t>(excess));
    }
    if (dsp_window_.size() >= 96U) {
        geophone_energy_trigger_config_t ecfg{};
        ecfg.noise_window_samples = 64U;
        ecfg.trigger_window_samples = 16U;
        ecfg.threshold_multiplier = 4.5f;
        geophone_energy_trigger_result_t eres{};
        if (geophone_event_energy_trigger_detect(dsp_window_.data(), dsp_window_.size(), &ecfg, &eres) &&
            eres.found) {
            report.dsp_triggers++;
        }
    }
}

void RuntimePipeline::maybe_send_uart_summary(pb2::transport::UartTransport& uart,
                                              const pb2::acquisition::SampleFrame& latest,
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
    hdr.session_epoch = cfg_.session_epoch;

    pb2_preview_block_t msg{};
    msg.first_sample_index = latest.source_sample_index;
    msg.first_timestamp_us = latest.timestamp_us;
    msg.source_sample_rate_hz = cfg_.sample_rate_hz;
    msg.decimation_factor = 1U;
    msg.frame_count = 1U;
    msg.clock_domain = (cfg_.source_kind == PipelineSourceKind::Mock)
                           ? PB2_CLOCK_PB2_LINUX_MONO_US
                           : PB2_CLOCK_PB2_PRU_TICKS;
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
    uart.poll_once(0, [&](const pb2_frame_header_t& rx_hdr, const uint8_t* /*rx_payload*/) {
        if (rx_hdr.msg_type == PB2_MSG_PING) {
            pb2_frame_header_t pong_hdr = rx_hdr;
            pong_hdr.msg_type = PB2_MSG_PONG;
            pong_hdr.sequence = uart_seq_++;
            pong_hdr.boot_epoch = cfg_.boot_epoch;
            pong_hdr.session_epoch = cfg_.session_epoch;
            pong_hdr.payload_len = 0U;
            uart.enqueue_frame(pong_hdr, nullptr);
        } else if (rx_hdr.msg_type == PB2_MSG_HELLO) {
            pb2_ack_payload_t ack{};
            ack.acked_sequence = rx_hdr.sequence;
            ack.acked_msg_type = rx_hdr.msg_type;
            ack.status_code = 0U;
            uint8_t ack_buf[16]{};
            uint16_t ack_len = 0U;
            if (pb2_encode_ack(&ack, ack_buf, sizeof(ack_buf), &ack_len) == PB2_PROTO_OK) {
                pb2_frame_header_t ack_hdr{};
                ack_hdr.version = PB2_PROTO_VERSION;
                ack_hdr.msg_type = PB2_MSG_ACK;
                ack_hdr.sequence = uart_seq_++;
                ack_hdr.boot_epoch = cfg_.boot_epoch;
                ack_hdr.session_epoch = cfg_.session_epoch;
                ack_hdr.payload_len = ack_len;
                uart.enqueue_frame(ack_hdr, ack_buf);
            }
        }
    });
    report.uart_tx_frames = uart.stats().frames_tx;
    report.uart_tx_drops = uart.stats().tx_queue_drops;
}

PipelineReport RuntimePipeline::run(const std::atomic<bool>& stop_requested) {
    PipelineReport report{};
    pb2::power::ShutdownFsm shutdown_fsm;

    pb2::storage::GeoseisV2Writer writer;
    const bool recording = !cfg_.record_base_path.empty();
    std::mutex writer_mtx;
    std::condition_variable writer_cv;
    std::deque<pb2::acquisition::SampleFrame> writer_queue;
    bool writer_stop = false;
    bool writer_failed = false;
    std::uint64_t writer_queue_drops = 0U;
    std::thread writer_thread;

    if (recording) {
        pb2::storage::GeoseisWriterConfig wcfg{};
        wcfg.base_path = cfg_.record_base_path;
        wcfg.sample_rate_hz = cfg_.sample_rate_hz;
        wcfg.enable_chunking = cfg_.enable_chunking;
        wcfg.allow_overwrite = cfg_.allow_overwrite;
        wcfg.max_chunk_bytes = cfg_.max_chunk_bytes;
        if (!writer.open(wcfg)) {
            report.error_message = "Failed to open GeoseisV2Writer at " + cfg_.record_base_path;
            return report;
        }
        report.recorded_file_path = writer.current_path();
        writer_thread = std::thread([&]() {
            std::vector<pb2::acquisition::SampleFrame> chunk;
            chunk.reserve(256U);
            for (;;) {
                chunk.clear();
                {
                    std::unique_lock<std::mutex> lk(writer_mtx);
                    writer_cv.wait(lk, [&]() { return writer_stop || !writer_queue.empty(); });
                    while (!writer_queue.empty() && chunk.size() < 256U) {
                        chunk.push_back(writer_queue.front());
                        writer_queue.pop_front();
                    }
                    if (chunk.empty() && writer_stop) {
                        break;
                    }
                }
                if (!chunk.empty() && !writer.write_frames(chunk.data(), chunk.size())) {
                    std::lock_guard<std::mutex> lk(writer_mtx);
                    writer_failed = true;
                    break;
                }
            }
        });
    }

    auto enqueue_recording = [&](const pb2::acquisition::SampleFrame* frames, std::size_t count) -> bool {
        if (!recording || count == 0U) {
            return true;
        }
        std::lock_guard<std::mutex> lk(writer_mtx);
        if (writer_failed) {
            report.error_message = "GeoseisV2Writer write_frames failed";
            return false;
        }
        const std::size_t cap = (cfg_.max_writer_queue_frames > 0U) ? cfg_.max_writer_queue_frames : 8192U;
        for (std::size_t i = 0; i < count; ++i) {
            if (writer_queue.size() >= cap) {
                writer_queue.pop_front();
                writer_queue_drops++;
            }
            writer_queue.push_back(frames[i]);
        }
        writer_cv.notify_one();
        return true;
    };

    pb2::transport::UartTransport uart(16U);
    if (cfg_.uart_fd >= 0) {
        uart.attach_fd(cfg_.uart_fd, false);
    } else if (!cfg_.uart_device.empty()) {
        uart.open_device(cfg_.uart_device, cfg_.uart_baud);
    }

    std::vector<pb2::acquisition::SampleFrame> batch;
    batch.reserve(64U);

    if (cfg_.source_kind == PipelineSourceKind::Mock) {
        pb2::acquisition::MockSourceConfig mcfg{};
        mcfg.sample_rate_hz = cfg_.sample_rate_hz;
        mcfg.profile = cfg_.profile;
        pb2::acquisition::MockAcquisitionSource source(mcfg);
        if (!source.is_valid()) {
            report.error_message = "Invalid MockSourceConfig";
            return report;
        }

        while (!stop_requested.load(std::memory_order_acquire) &&
               (cfg_.max_frames == 0U || report.frames_processed < cfg_.max_frames)) {
            const std::size_t want = (cfg_.max_frames == 0U)
                                         ? 64U
                                         : std::min<std::size_t>(64U, cfg_.max_frames - report.frames_processed);
            batch.clear();
            if (!source.read_frames(want, batch) || batch.empty()) {
                break;
            }
            if (!enqueue_recording(batch.data(), batch.size())) {
                break;
            }
            push_preview(batch.data(), batch.size());
            run_dsp_window(batch.data(), batch.size(), report);
            maybe_send_uart_summary(uart, batch.back(), report);
            report.frames_processed += batch.size();
        }
        report.dropped_frames = source.stats().dropped_frames;
    } else {
        pb2::acquisition::PruSharedMemory shm;
        bool opened = false;
        const bool is_live = (cfg_.source_kind != PipelineSourceKind::PruSnapshotFile);
        if (cfg_.source_kind == PipelineSourceKind::PruSnapshotFile) {
            opened = shm.open_file(cfg_.pru_path, false);
        } else if (cfg_.source_kind == PipelineSourceKind::PruLiveFile) {
            opened = shm.open_file(cfg_.pru_path, true);
        } else if (cfg_.source_kind == PipelineSourceKind::PruUio) {
            opened = shm.open_uio(cfg_.pru_uio_dev);
        } else {
            opened = shm.open_dev_mem(cfg_.pru_phys_addr, true);
        }
        if (!opened) {
            report.error_message = "Failed to open PRU shared memory backend";
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
            return report;
        }

        if (is_live) {
            if (!consumer.start_acquisition(cfg_.sample_rate_hz,
                                            cfg_.profile,
                                            200000000U,
                                            1U,
                                            cfg_.pru_cmd_timeout_ms)) {
                report.error_message = "PRU lifecycle CONFIGURE->ARM->START failed";
                return report;
            }
        }

        auto process_batch = [&](const std::vector<pb2::acquisition::SampleFrame>& frames) -> bool {
            if (frames.empty()) {
                return true;
            }
            if (!enqueue_recording(frames.data(), frames.size())) {
                return false;
            }
            push_preview(frames.data(), frames.size());
            run_dsp_window(frames.data(), frames.size(), report);
            maybe_send_uart_summary(uart, frames.back(), report);
            report.frames_processed += frames.size();
            return true;
        };

        while (!stop_requested.load(std::memory_order_acquire)) {
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
        report.dropped_frames = pstats.dropped_frames;
        report.gap_blocks = pstats.gap_blocks;
    }

    shutdown_fsm.on_event(pb2::power::ShutdownEvent::RequestStop);
    shutdown_fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped);
    if (recording) {
        {
            std::lock_guard<std::mutex> lk(writer_mtx);
            writer_stop = true;
        }
        writer_cv.notify_all();
        if (writer_thread.joinable()) {
            writer_thread.join();
        }
        report.dropped_frames += writer_queue_drops;
        if (writer_failed && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer write_frames failed";
        }
        if (!writer.flush() && report.error_message.empty()) {
            report.error_message = "GeoseisV2Writer flush/fsync failed";
        }
        writer.close();
    }
    shutdown_fsm.on_event(pb2::power::ShutdownEvent::StorageFlushed);
    shutdown_fsm.on_event(pb2::power::ShutdownEvent::SessionFinalized);
    shutdown_fsm.on_event(pb2::power::ShutdownEvent::PruStopped);

    if (uart.is_open()) {
        report.uart_tx_frames = uart.stats().frames_tx;
        report.uart_tx_drops = uart.stats().tx_queue_drops;
        uart.close_device();
    }

    report.preview_frames_kept = preview_ring_.size();
    report.shutdown_state = shutdown_fsm.state();
    report.ok = report.error_message.empty();
    return report;
}

} // namespace pb2::runtime
