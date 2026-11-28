#include "pb2/runtime/pipeline.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pty.h>
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
    cfg.source_kind = pb2::runtime::PipelineSourceKind::PruFile;
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

} // namespace

int main() {
    test_mock_pipeline_records_geoseis_and_uart();
    test_pru_file_dump_pipeline_and_signal_stop();
    std::printf("pb2_pipeline_tests: ALL PASSED\n");
    return 0;
}
