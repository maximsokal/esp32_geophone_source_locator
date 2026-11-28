#include "pb2/runtime/pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pty.h>
#include <thread>
#include <unistd.h>

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

} // namespace

int main() {
    test_mock_pipeline_records_geoseis_and_uart();
    test_pru_file_dump_pipeline_and_signal_stop();
    test_live_pru_lifecycle_with_empty_ring_wait();
    std::printf("pb2_pipeline_tests: ALL PASSED\n");
    return 0;
}
