#include "pb2/runtime/pipeline.hpp"

extern "C" {
#include "geophone_events.h"
#include "pb2_protocol.h"
}

#include <algorithm>
#include <cstring>
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
    hdr.boot_epoch = 1U;
    hdr.session_epoch = 1U;

    pb2_preview_block_t msg{};
    msg.first_sample_index = latest.source_sample_index;
    msg.first_timestamp_us = latest.timestamp_us;
    msg.source_sample_rate_hz = cfg_.sample_rate_hz;
    msg.decimation_factor = 1U;
    msg.frame_count = 1U;
    msg.clock_domain = PB2_CLOCK_PB2_LINUX_MONO_US;
    msg.sample_format = PB2_SAMPLE_FMT_FLOAT32_UV;
    msg.channel_count = PB2_PROTO_CHANNEL_COUNT;
    for (std::size_t ch = 0; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        msg.channel_map[ch] = static_cast<uint8_t>(ch);
        msg.samples_uv[0][ch] = static_cast<float>(latest.channels[ch]);
    }

    uint8_t payload_buf[PB2_PROTO_MAX_PAYLOAD];
    uint16_t payload_len = 0U;
    if (pb2_encode_preview_block(&msg, payload_buf, sizeof(payload_buf), &payload_len) != PB2_PROTO_OK) {
        return;
    }
    hdr.payload_len = payload_len;
    uart.enqueue_frame(hdr, payload_buf);
    uart.poll_once(0, [](const pb2_frame_header_t&, const uint8_t*) {});
    report.uart_tx_frames = uart.stats().frames_tx;
    report.uart_tx_drops = uart.stats().tx_queue_drops;
}

PipelineReport RuntimePipeline::run(const std::atomic<bool>& stop_requested) {
    PipelineReport report{};
    pb2::power::ShutdownFsm shutdown_fsm;

    pb2::storage::GeoseisV2Writer writer;
    const bool recording = !cfg_.record_base_path.empty();
    if (recording) {
        pb2::storage::GeoseisWriterConfig wcfg{};
        wcfg.base_path = cfg_.record_base_path;
        wcfg.sample_rate_hz = cfg_.sample_rate_hz;
        wcfg.enable_chunking = cfg_.enable_chunking;
        wcfg.max_chunk_bytes = cfg_.max_chunk_bytes;
        if (!writer.open(wcfg)) {
            report.error_message = "Failed to open GeoseisV2Writer at " + cfg_.record_base_path;
            return report;
        }
        report.recorded_file_path = writer.current_path();
    }

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
            if (recording && !writer.write_frames(batch.data(), batch.size())) {
                report.error_message = "GeoseisV2Writer write_frames failed";
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
        const bool opened = (cfg_.source_kind == PipelineSourceKind::PruFile)
                                ? shm.open_file(cfg_.pru_path, false)
                                : shm.open_dev_mem(cfg_.pru_phys_addr, true);
        if (!opened) {
            report.error_message = "Failed to open PRU shared memory backend";
            return report;
        }
        pb2::acquisition::PruRingConsumer consumer(shm.get());
        if (!consumer.is_valid()) {
            report.error_message = "Invalid PRU shared memory header (magic/version/channels)";
            return report;
        }

        while (!stop_requested.load(std::memory_order_acquire)) {
            batch.clear();
            const std::size_t got = consumer.drain(16U, batch);
            if (got == 0U) {
                break;
            }
            if (recording && !writer.write_frames(batch.data(), batch.size())) {
                report.error_message = "GeoseisV2Writer write_frames failed";
                break;
            }
            push_preview(batch.data(), batch.size());
            run_dsp_window(batch.data(), batch.size(), report);
            maybe_send_uart_summary(uart, batch.back(), report);
            report.frames_processed += batch.size();
            if (cfg_.max_frames > 0U && report.frames_processed >= cfg_.max_frames) {
                break;
            }
        }
        const auto pstats = consumer.snapshot_stats();
        report.dropped_frames = pstats.dropped_frames;
        report.gap_blocks = pstats.gap_blocks;
    }

    shutdown_fsm.on_event(pb2::power::ShutdownEvent::RequestStop);
    shutdown_fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped);
    if (recording) {
        writer.flush();
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
