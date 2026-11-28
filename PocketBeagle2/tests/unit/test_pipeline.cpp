#include "pb2/runtime/pipeline.hpp"
#include "pb2/hardware/pinmap_a0.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define P_CHECK(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Pipeline check failed: %s (%s:%d)\n",     \
                         #cond, __FILE__, __LINE__);                        \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

void test_mock_pipeline_records_geoseis_and_uart() {
    int master_fd = -1;
    int slave_fd = -1;
    P_CHECK(::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) == 0);

    const std::string out_base = "/tmp/pb2_pipeline_mock";
    std::remove((out_base + ".bin").c_str());

    pb2::runtime::PipelineConfig cfg{};
    cfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
    cfg.sample_rate_hz = 20000U;
    cfg.max_frames = 256U;
    cfg.record_base_path = out_base;
    cfg.enable_chunking = false;
    cfg.enable_dsp = true;
    cfg.preview_capacity = 32U;
    cfg.uart_fd = slave_fd;

    std::atomic<bool> stop{false};
    pb2::runtime::RuntimePipeline pipeline(cfg);
    const auto report = pipeline.run(stop);

    P_CHECK(report.ok);
    P_CHECK(report.frames_processed == 256U);
    P_CHECK(report.preview_frames_kept == 32U);
    P_CHECK(report.uart_tx_frames > 0U);
    P_CHECK(report.shutdown_state == pb2::power::ShutdownState::SyncFilesystem);

    const auto rec = pb2::storage::recover_geoseis_v2_file(report.recorded_file_path);
    P_CHECK(rec.ok);
    P_CHECK(rec.valid_frames == 256U);

    ::close(master_fd);
    ::close(slave_fd);
    std::remove(report.recorded_file_path.c_str());
}

void test_pru_file_dump_pipeline_and_signal_stop() {
    auto shm = std::make_unique<pb2_pru_shared_t>();
    std::memset(shm.get(), 0, sizeof(pb2_pru_shared_t));
    shm->magic = PB2_PRU_RING_MAGIC;
    shm->version = PB2_PRU_RING_VERSION;
    shm->channel_count = PB2_PRU_CHANNEL_COUNT;
    shm->profile = PB2_PRU_PROFILE_DUAL_DOUT;
    shm->sample_rate_hz = 100000U;
    shm->iep_clock_hz = 200000000U;

    for (std::uint32_t b = 0; b < 8U; ++b) {
        auto& blk = shm->blocks[b];
        blk.first_sample_index = static_cast<std::uint64_t>(b) * PB2_PRU_BLOCK_FRAMES;
        blk.first_iep_ticks = blk.first_sample_index * 2000ULL;
        blk.sequence = b;
        blk.frame_count = PB2_PRU_BLOCK_FRAMES;
        for (std::uint16_t f = 0; f < PB2_PRU_BLOCK_FRAMES; ++f) {
            blk.channels[f][0] = static_cast<std::int16_t>(100 + f);
            blk.channels[f][1] = static_cast<std::int16_t>(-100 - f);
        }
    }
    shm->write_block_idx = 8U;

    const char* pru_dump = "/tmp/pb2_pipeline_pru_dump.bin";
    const std::string out_base = "/tmp/pb2_pipeline_pru_rec";
    std::remove((out_base + ".bin").c_str());

    std::FILE* fp = std::fopen(pru_dump, "wb");
    P_CHECK(fp != nullptr);
    P_CHECK(std::fwrite(shm.get(), sizeof(pb2_pru_shared_t), 1, fp) == 1);
    std::fclose(fp);

    pb2::runtime::PipelineConfig cfg{};
    cfg.source_kind = pb2::runtime::PipelineSourceKind::PruSnapshotFile;
    cfg.pru_path = pru_dump;
    cfg.sample_rate_hz = 100000U;
    cfg.max_frames = 0U;
    cfg.record_base_path = out_base;
    cfg.enable_chunking = false;

    std::atomic<bool> stop{false};
    pb2::runtime::RuntimePipeline pipeline(cfg);
    const auto report = pipeline.run(stop);

    P_CHECK(report.ok);
    P_CHECK(report.frames_processed == 8U * PB2_PRU_BLOCK_FRAMES);

    const auto rec = pb2::storage::recover_geoseis_v2_file(report.recorded_file_path);
    P_CHECK(rec.ok);
    P_CHECK(rec.valid_frames == 8U * PB2_PRU_BLOCK_FRAMES);

    std::remove(pru_dump);
    std::remove(report.recorded_file_path.c_str());
}

void test_live_pru_lifecycle_with_empty_ring_wait() {
    const char* live_path = "/tmp/pb2_pipeline_live_shm.bin";
    {
        auto init = std::make_unique<pb2_pru_shared_t>();
        std::memset(init.get(), 0, sizeof(pb2_pru_shared_t));
        init->magic = PB2_PRU_RING_MAGIC;
        init->version = PB2_PRU_RING_VERSION;
        init->channel_count = PB2_PRU_CHANNEL_COUNT;
        std::FILE* fp = std::fopen(live_path, "wb");
        P_CHECK(fp != nullptr);
        P_CHECK(std::fwrite(init.get(), sizeof(pb2_pru_shared_t), 1, fp) == 1);
        std::fclose(fp);
    }

    pb2::acquisition::PruSharedMemory pru_side;
    P_CHECK(pru_side.open_file(live_path, true));
    auto* shm = pru_side.get();

    std::atomic<bool> pru_done{false};
    std::thread pru_sim([&]() {
        std::uint32_t ack = 0;
        bool acquiring = false;
        std::uint32_t seq = 0;
        while (!pru_done.load(std::memory_order_acquire)) {
            const std::uint32_t cmd_seq = __atomic_load_n(&shm->cmd_seq, __ATOMIC_ACQUIRE);
            if (cmd_seq != ack) {
                const std::uint32_t cmd = shm->cmd;
                if (cmd == PB2_PRU_CMD_CONFIGURE) {
                    __atomic_store_n(&shm->state, PB2_PRU_STATE_CONFIGURED, __ATOMIC_RELEASE);
                } else if (cmd == PB2_PRU_CMD_ARM) {
                    __atomic_store_n(&shm->state, PB2_PRU_STATE_ARMED, __ATOMIC_RELEASE);
                } else if (cmd == PB2_PRU_CMD_START) {
                    __atomic_store_n(&shm->state, PB2_PRU_STATE_ACQUIRING, __ATOMIC_RELEASE);
                    acquiring = true;
                } else if (cmd == PB2_PRU_CMD_STOP) {
                    acquiring = false;
                    // Publish 1 drained block during STOP -> DRAINING -> STOPPED
                    const std::uint32_t w = __atomic_load_n(&shm->write_block_idx, __ATOMIC_RELAXED);
                    auto& blk = shm->blocks[w];
                    blk.first_sample_index = static_cast<std::uint64_t>(seq) * PB2_PRU_BLOCK_FRAMES;
                    blk.first_iep_ticks = blk.first_sample_index * 2000ULL;
                    blk.sequence = seq++;
                    blk.frame_count = PB2_PRU_BLOCK_FRAMES;
                    __atomic_store_n(&shm->write_block_idx, (w + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U), __ATOMIC_RELEASE);
                    __atomic_store_n(&shm->state, PB2_PRU_STATE_STOPPED, __ATOMIC_RELEASE);
                    __atomic_store_n(&shm->last_cmd_ack, cmd_seq, __ATOMIC_RELEASE);
                    ack = cmd_seq;
                    break;
                }
                __atomic_store_n(&shm->last_cmd_ack, cmd_seq, __ATOMIC_RELEASE);
                ack = cmd_seq;
            }
            if (acquiring && seq < 3U) {
                // Delay before first block so consumer sees empty ring first and does NOT exit
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const std::uint32_t w = __atomic_load_n(&shm->write_block_idx, __ATOMIC_RELAXED);
                auto& blk = shm->blocks[w];
                blk.first_sample_index = static_cast<std::uint64_t>(seq) * PB2_PRU_BLOCK_FRAMES;
                blk.first_iep_ticks = blk.first_sample_index * 2000ULL;
                blk.sequence = seq++;
                blk.frame_count = PB2_PRU_BLOCK_FRAMES;
                __atomic_store_n(&shm->write_block_idx, (w + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U), __ATOMIC_RELEASE);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    const std::string out_base = "/tmp/pb2_pipeline_live_rec.bin";
    std::remove(out_base.c_str());

    pb2::runtime::PipelineConfig cfg{};
    cfg.source_kind = pb2::runtime::PipelineSourceKind::PruLiveFile;
    cfg.pru_path = live_path;
    cfg.sample_rate_hz = 100000U;
    cfg.max_frames = 3U * PB2_PRU_BLOCK_FRAMES;
    cfg.record_base_path = out_base;
    cfg.enable_chunking = false;
    cfg.pru_cmd_timeout_ms = 2000U;

    std::atomic<bool> stop{false};
    pb2::runtime::RuntimePipeline pipeline(cfg);
    const auto report = pipeline.run(stop);
    pru_done.store(true, std::memory_order_release);
    pru_sim.join();

    P_CHECK(report.ok);
    // 3 blocks during acquire + 1 drained block on STOP = 4 blocks
    P_CHECK(report.frames_processed == 4U * PB2_PRU_BLOCK_FRAMES);
    std::remove(live_path);
    std::remove(out_base.c_str());
}

void test_failure_paths_raii_no_terminate() {
    std::atomic<bool> stop{false};

    /* 1. Invalid mock config with active recording (must join worker cleanly, not terminate) */
    {
        const std::string p = "/tmp/pb2_fail_mock.bin";
        std::remove(p.c_str());
        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
        cfg.sample_rate_hz = 0U; /* invalid */
        cfg.record_base_path = p;
        pb2::runtime::RuntimePipeline pipe(cfg);
        const auto rep = pipe.run(stop);
        P_CHECK(!rep.ok);
        P_CHECK(rep.shutdown_state == pb2::power::ShutdownState::Fault);
        std::remove(p.c_str());
    }

    /* 2. Invalid PRU header with active recording */
    {
        const char* bad_pru = "/tmp/pb2_bad_pru.bin";
        const std::string p = "/tmp/pb2_fail_pru_rec.bin";
        std::remove(p.c_str());
        std::vector<std::uint8_t> zeros(sizeof(pb2_pru_shared_t), 0U);
        std::FILE* fp = std::fopen(bad_pru, "wb");
        P_CHECK(fp != nullptr);
        std::fwrite(zeros.data(), 1, zeros.size(), fp);
        std::fclose(fp);

        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::PruSnapshotFile;
        cfg.pru_path = bad_pru;
        cfg.record_base_path = p;
        pb2::runtime::RuntimePipeline pipe(cfg);
        const auto rep = pipe.run(stop);
        P_CHECK(!rep.ok);
        P_CHECK(rep.shutdown_state == pb2::power::ShutdownState::Fault);
        std::remove(bad_pru);
        std::remove(p.c_str());
    }

    /* 3. Missing UIO / invalid PRU clock */
    {
        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::PruUio;
        cfg.pru_uio_dev = "nonexistent_uio_device_xyz";
        pb2::runtime::RuntimePipeline pipe(cfg);
        const auto rep = pipe.run(stop);
        P_CHECK(!rep.ok);
        P_CHECK(rep.shutdown_state == pb2::power::ShutdownState::Fault);
    }

    /* 4. Preallocated DDR ring overflow under SD stall marks degraded=true and ok=false */
    {
        const std::string p = "/tmp/pb2_overflow_rec.bin";
        std::remove(p.c_str());
        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
        cfg.sample_rate_hz = 20000U;
        cfg.max_frames = 512U;
        cfg.max_writer_queue_frames = 32U;
        cfg.writer_stall_inject_ms = 5U;
        cfg.record_base_path = p;
        pb2::runtime::RuntimePipeline pipe(cfg);
        const auto rep = pipe.run(stop);
        P_CHECK(rep.frames_acquired == 512U);
        P_CHECK(rep.degraded);
        P_CHECK(!rep.ok);
        P_CHECK(rep.writer_queue_dropped > 0U);
        P_CHECK(rep.frames_written + rep.writer_queue_dropped == rep.frames_acquired);
        P_CHECK(!rep.shutdown_ready_sent);
        std::remove(p.c_str());
    }
}

void test_hardware_clock_probe_cases() {
    std::atomic<bool> stop{false};

    /* 1. Missing clock debugfs blocks PruUio */
    {
        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::PruUio;
        cfg.clk_debug_root = "/tmp/nonexistent_clk_debug_xyz";
        pb2::runtime::RuntimePipeline pipe(cfg);
        const auto rep = pipe.run(stop);
        P_CHECK(!rep.ok);
        P_CHECK(rep.shutdown_state == pb2::power::ShutdownState::Fault);
    }

    /* 2. Invalid PRU clock (150 MHz) blocks PruUio */
    {
        const std::string clk_root = "/tmp/pb2_test_clk_bad";
        P_CHECK(std::system(("mkdir -p " + clk_root + "/pruss_coreclk_mux " + clk_root + "/pruss_iepclk_mux").c_str()) == 0);
        {
            std::ofstream f1(clk_root + "/pruss_coreclk_mux/clk_rate");
            f1 << "150000000\n";
            std::ofstream f2(clk_root + "/pruss_iepclk_mux/clk_rate");
            f2 << "200000000\n";
        }
        const auto res = pb2::hardware::probe_hardware_pru_iep_clocks(clk_root);
        P_CHECK(!res.valid);
        P_CHECK(res.pru_clock_hz == 150000000U);
        P_CHECK(std::system(("rm -rf " + clk_root).c_str()) == 0);
    }

    /* 3. Valid 200 MHz PRU + IEP clock passes probe */
    {
        const std::string clk_root = "/tmp/pb2_test_clk_ok";
        P_CHECK(std::system(("mkdir -p " + clk_root + "/pruss_coreclk_mux " + clk_root + "/pruss_iepclk_mux").c_str()) == 0);
        {
            std::ofstream f1(clk_root + "/pruss_coreclk_mux/clk_rate");
            f1 << "200000000\n";
            std::ofstream f2(clk_root + "/pruss_iepclk_mux/clk_rate");
            f2 << "200000000\n";
        }
        const auto res = pb2::hardware::probe_hardware_pru_iep_clocks(clk_root);
        P_CHECK(res.valid);
        P_CHECK(res.pru_clock_hz == 200000000U);
        P_CHECK(res.iep_clock_hz == 200000000U);
        P_CHECK(std::system(("rm -rf " + clk_root).c_str()) == 0);
    }
}

void test_safe_shutdown_order_and_duplicate_request() {
    int pty_master = ::posix_openpt(O_RDWR | O_NOCTTY);
    P_CHECK(pty_master >= 0);
    P_CHECK(::grantpt(pty_master) == 0);
    P_CHECK(::unlockpt(pty_master) == 0);
    const char* slave = ::ptsname(pty_master);
    P_CHECK(slave != nullptr);

    const std::string rec_path = "/tmp/pb2_safe_shutdown_rec.bin";
    std::remove(rec_path.c_str());

    pb2::runtime::PipelineConfig cfg{};
    cfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
    cfg.sample_rate_hz = 20000U;
    cfg.max_frames = 256U;
    cfg.record_base_path = rec_path;
    cfg.uart_device = slave;
    cfg.uart_baud = 115200U;

    /* Send duplicate SHUTDOWN_REQUEST frames from ESP32 peer */
    auto send_shutdown_req = [&](std::uint32_t seq) {
        pb2_frame_header_t hdr{PB2_PROTO_VERSION, PB2_MSG_SHUTDOWN_REQUEST, 0U, seq, 5U, 9U, 0U};
        std::uint8_t wire[PB2_PROTO_MAX_COBS_BYTES]{};
        size_t wlen = 0U;
        P_CHECK(pb2_wire_encode(&hdr, nullptr, wire, sizeof(wire), &wlen) == PB2_PROTO_OK);
        P_CHECK(::write(pty_master, wire, wlen) == static_cast<ssize_t>(wlen));
    };
    send_shutdown_req(1U);
    send_shutdown_req(2U);

    pb2::runtime::RuntimePipeline pipe(cfg);
    std::atomic<bool> stop{false};
    const auto rep = pipe.run(stop);
    P_CHECK(rep.ok);
    P_CHECK(rep.shutdown_ready_sent);
    P_CHECK(rep.active_boot_epoch >= 2U);
    P_CHECK(rep.active_session_epoch >= 2U);
    /* Must NOT claim SafePowerOff/KernelHalted just because SHUTDOWN_READY was sent */
    P_CHECK(rep.shutdown_state == pb2::power::ShutdownState::SyncFilesystem);

    /* Verify ACK arrives BEFORE SHUTDOWN_READY */
    std::vector<std::uint8_t> rx_order;
    pb2_stream_decoder_t dec{};
    pb2_stream_decoder_init(&dec);
    std::uint8_t buf[1024]{};
    for (int i = 0; i < 10; ++i) {
        const ssize_t n = ::read(pty_master, buf, sizeof(buf));
        if (n > 0) {
            pb2_stream_decoder_feed(
                &dec, buf, static_cast<size_t>(n),
                [](const pb2_frame_header_t* h, const uint8_t*, void* ctx) {
                    auto* vec = static_cast<std::vector<std::uint8_t>*>(ctx);
                    vec->push_back(h->msg_type);
                },
                &rx_order);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::close(pty_master);
    std::remove(rec_path.c_str());

    bool saw_ack = false;
    bool saw_ready = false;
    for (const auto t : rx_order) {
        if (t == PB2_MSG_ACK) saw_ack = true;
        if (t == PB2_MSG_SHUTDOWN_READY) {
            P_CHECK(saw_ack); /* ACK must precede SHUTDOWN_READY */
            saw_ready = true;
        }
    }
    P_CHECK(saw_ack && saw_ready);
}

void test_geometry_required_and_golden_localization_fixture() {
    /* 1. Without geometry: measurements recorded, NO fake localization */
    {
        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::Mock;
        cfg.sample_rate_hz = 20000U;
        cfg.max_frames = 256U;
        cfg.enable_dsp = true;
        pb2::runtime::RuntimePipeline pipe(cfg);
        std::atomic<bool> stop{false};
        const auto rep = pipe.run(stop);
        P_CHECK(rep.ok);
        P_CHECK(rep.dsp_localizations == 0U);
        P_CHECK(!rep.last_localization.valid);
    }

    /* 2. Golden 3D fixture: known non-coplanar geometry + source -> TDOA + coords + residual */
    {
        int pty_master = ::posix_openpt(O_RDWR | O_NOCTTY);
        P_CHECK(pty_master >= 0);
        P_CHECK(::grantpt(pty_master) == 0);
        P_CHECK(::unlockpt(pty_master) == 0);
        const char* slave = ::ptsname(pty_master);
        P_CHECK(slave != nullptr);

        const std::string dump_path = "/tmp/pb2_golden_loc_shm.bin";
        std::remove(dump_path.c_str());

        pb2_pru_shared_t shm{};
        shm.magic = PB2_PRU_RING_MAGIC;
        shm.version = PB2_PRU_RING_VERSION;
        shm.channel_count = 4U;
        shm.profile = PB2_PRU_PROFILE_DUAL_DOUT;
        shm.state = PB2_PRU_STATE_STOPPED;
        shm.sample_rate_hz = 20000U;
        shm.session_epoch = 42U;

        const double sensors[4][3] = {
            {0.0, 0.0, 0.0},
            {6.0, 0.0, 0.0},
            {0.0, 6.0, 0.0},
            {3.0, 3.0, 4.0},
        };
        const double src[3] = {2.5, 2.0, 1.5};
        const double speed_mps = 1000.0;

        /* Compute exact arrival sample indices for each sensor */
        double dist[4]{};
        double min_d = 1e9;
        for (int s = 0; s < 4; ++s) {
            const double dx = src[0] - sensors[s][0];
            const double dy = src[1] - sensors[s][1];
            const double dz = src[2] - sensors[s][2];
            dist[s] = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist[s] < min_d) min_d = dist[s];
        }
        std::size_t trig_sample[4]{};
        for (int s = 0; s < 4; ++s) {
            trig_sample[s] = 120U + static_cast<std::size_t>(
                std::llround(((dist[s] - min_d) / speed_mps) * 20000.0));
        }

        /* Synthesize 640 frames (40 blocks x 16 frames <= 64-block capacity) */
        constexpr std::size_t kBlocks = 40U;
        for (std::size_t b = 0; b < kBlocks; ++b) {
            pb2_pru_block_t blk{};
            blk.first_sample_index = b * PB2_PRU_BLOCK_FRAMES;
            blk.first_iep_ticks = blk.first_sample_index * 10000ULL;
            blk.sequence = static_cast<std::uint32_t>(b + 1U);
            blk.frame_count = PB2_PRU_BLOCK_FRAMES;
            for (std::size_t f = 0; f < PB2_PRU_BLOCK_FRAMES; ++f) {
                const std::size_t idx = blk.first_sample_index + f;
                for (int ch = 0; ch < 4; ++ch) {
                    const std::int16_t baseline = static_cast<std::int16_t>((idx % 2 == 0) ? 5 : -5);
                    const bool in_burst = (idx >= trig_sample[ch] && idx < trig_sample[ch] + 16U);
                    blk.channels[f][ch] = in_burst ? static_cast<std::int16_t>(8000) : baseline;
                }
            }
            shm.blocks[b % PB2_PRU_BLOCK_CAPACITY] = blk;
        }
        shm.write_block_idx = kBlocks;
        shm.total_frames = kBlocks * PB2_PRU_BLOCK_FRAMES;
        {
            std::ofstream ofs(dump_path, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(&shm), sizeof(shm));
        }

        pb2::runtime::PipelineConfig cfg{};
        cfg.source_kind = pb2::runtime::PipelineSourceKind::PruSnapshotFile;
        cfg.pru_path = dump_path;
        cfg.sample_rate_hz = 20000U;
        cfg.enable_dsp = true;
        cfg.uart_device = slave;
        cfg.initial_geometry.valid = true;
        cfg.initial_geometry.geometry_source = PB2_GEOMETRY_SRC_APRILTAG_3D;
        cfg.initial_geometry.session_epoch = 42U;
        cfg.initial_geometry.propagation_speed_mps = speed_mps;
        for (int s = 0; s < 4; ++s) {
            for (int k = 0; k < 3; ++k) {
                cfg.initial_geometry.sensor_positions_m[s][k] = sensors[s][k];
            }
            cfg.initial_geometry.channel_to_sensor[s] = static_cast<std::uint8_t>(s);
        }

        pb2::runtime::RuntimePipeline pipe(cfg);
        std::atomic<bool> stop{false};
        const auto rep = pipe.run(stop);
        P_CHECK(rep.ok);
        P_CHECK(rep.dsp_window_samples > 256U);
        P_CHECK(rep.dsp_localizations >= 1U);
        P_CHECK(rep.last_localization.valid);
        P_CHECK(rep.last_localization.geometry_source == PB2_GEOMETRY_SRC_APRILTAG_3D);
        P_CHECK(std::fabs(rep.last_localization.source_x_m - src[0]) < 0.25);
        P_CHECK(std::fabs(rep.last_localization.source_y_m - src[1]) < 0.25);
        P_CHECK(std::fabs(rep.last_localization.source_z_m - src[2]) < 0.25);
        P_CHECK(rep.last_localization.rms_residual_us < 25.0);

        /* Verify PB2_MSG_LOCALIZATION_RESULT was transmitted over UART */
        bool saw_loc_msg = false;
        pb2_stream_decoder_t dec{};
        pb2_stream_decoder_init(&dec);
        std::uint8_t ubuf[2048]{};
        for (int i = 0; i < 10 && !saw_loc_msg; ++i) {
            const ssize_t n = ::read(pty_master, ubuf, sizeof(ubuf));
            if (n > 0) {
                pb2_stream_decoder_feed(
                    &dec, ubuf, static_cast<size_t>(n),
                    [](const pb2_frame_header_t* h, const uint8_t* p, void* ctx) {
                        if (h->msg_type == PB2_MSG_LOCALIZATION_RESULT) {
                            pb2_localization_result_t lr{};
                            P_CHECK(pb2_decode_localization_result(p, h->payload_len, &lr) == PB2_PROTO_OK);
                            P_CHECK(lr.valid == 1U);
                            P_CHECK(h->session_epoch == 42U);
                            *static_cast<bool*>(ctx) = true;
                        }
                    },
                    &saw_loc_msg);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        P_CHECK(saw_loc_msg);
        ::close(pty_master);
        std::remove(dump_path.c_str());
    }
}

void test_pru_ring_overflow_accounting() {
    pb2_pru_shared_t shm{};
    shm.magic = PB2_PRU_RING_MAGIC;
    shm.version = PB2_PRU_RING_VERSION;
    shm.channel_count = 4U;
    shm.state = PB2_PRU_STATE_STOPPED;
    shm.sample_rate_hz = 20000U;
    shm.session_epoch = 77U;

    /* Simulate Linux consumer stall: PRU wrapped ring, dropped 20 blocks (320 frames) */
    constexpr std::uint32_t kSurvivingBlocks = 60U;
    for (std::uint32_t b = 0; b < kSurvivingBlocks; ++b) {
        pb2_pru_block_t blk{};
        blk.first_sample_index = static_cast<std::uint64_t>(b + 20U) * PB2_PRU_BLOCK_FRAMES;
        blk.sequence = b + 21U;
        blk.frame_count = PB2_PRU_BLOCK_FRAMES;
        shm.blocks[b % PB2_PRU_BLOCK_CAPACITY] = blk;
    }
    shm.write_block_idx = kSurvivingBlocks;
    shm.read_block_idx = 0U;
    shm.dropped_frames = 20ULL * PB2_PRU_BLOCK_FRAMES;
    shm.overrun_count = 20ULL;
    shm.total_frames = static_cast<std::uint64_t>(kSurvivingBlocks) * PB2_PRU_BLOCK_FRAMES;

    const std::string dump_path = "/tmp/pb2_pru_overflow_shm.bin";
    const std::string rec_path = "/tmp/pb2_pru_overflow_rec.bin";
    std::remove(dump_path.c_str());
    std::remove(rec_path.c_str());
    {
        std::ofstream ofs(dump_path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(&shm), sizeof(shm));
    }

    pb2::runtime::PipelineConfig cfg{};
    cfg.source_kind = pb2::runtime::PipelineSourceKind::PruSnapshotFile;
    cfg.pru_path = dump_path;
    cfg.record_base_path = rec_path;
    cfg.sample_rate_hz = 20000U;
    cfg.enable_dsp = false;
    cfg.max_frames = 0U;

    pb2::runtime::RuntimePipeline pipe(cfg);
    std::atomic<bool> stop{false};
    const auto rep = pipe.run(stop);
    P_CHECK(rep.pru_ring_overflow_dropped > 0U);
    P_CHECK(rep.writer_queue_dropped == 0U);
    P_CHECK(rep.frames_acquired == rep.frames_written);
    P_CHECK(rep.degraded);
    P_CHECK(!rep.ok);
    std::remove(dump_path.c_str());
    std::remove(rec_path.c_str());
}

} // namespace

int main() {
    test_mock_pipeline_records_geoseis_and_uart();
    test_pru_file_dump_pipeline_and_signal_stop();
    test_live_pru_lifecycle_with_empty_ring_wait();
    test_failure_paths_raii_no_terminate();
    test_hardware_clock_probe_cases();
    test_safe_shutdown_order_and_duplicate_request();
    test_geometry_required_and_golden_localization_fixture();
    test_pru_ring_overflow_accounting();
    std::printf("pb2_pipeline_tests: ALL PASSED\n");
    return 0;
}
