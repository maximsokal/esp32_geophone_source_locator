#include "pb2/storage/geoseis_v2_writer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#define S_CHECK(cond)                                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Storage check failed: %s (%s:%d)\n",      \
                         #cond, __FILE__, __LINE__);                        \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

void test_geoseis_v2_layout_gaps_and_recovery() {
    const std::string base = "/tmp/pb2_test_geoseis_v2";
    const std::string file0 = base + "_000.bin";
    const std::string file1 = base + "_001.bin";
    ::unlink(file0.c_str());
    ::unlink(file1.c_str());

    pb2::storage::GeoseisWriterConfig cfg{};
    cfg.base_path = base;
    cfg.sample_rate_hz = 200000U;
    cfg.max_chunk_bytes = 2500U; /* force chunk rollover after ~1 block */
    cfg.min_free_bytes = 0U;
    cfg.enable_chunking = true;

    pb2::storage::GeoseisV2Writer writer;
    S_CHECK(writer.open(cfg));

    pb2::acquisition::MockSourceConfig mcfg{};
    mcfg.sample_rate_hz = 200000U;
    mcfg.inject_drop_every_n_frames = 300U;
    pb2::acquisition::MockAcquisitionSource src(mcfg);

    std::vector<pb2::acquisition::SampleFrame> frames;
    S_CHECK(src.read_frames(400, frames));
    S_CHECK(writer.write_frames(frames.data(), frames.size()));
    S_CHECK(writer.flush());
    writer.close();

    S_CHECK(writer.frames_written() == 400ULL);
    S_CHECK(writer.gap_count() >= 1ULL);
    S_CHECK(writer.chunk_index() >= 1U);

    /* Inject torn trailing bytes into file0 and verify recovery truncates cleanly */
    const int fd = ::open(file0.c_str(), O_WRONLY | O_APPEND);
    S_CHECK(fd >= 0);
    const std::uint8_t garbage[17] = {0xAA, 0xBB, 0xCC};
    S_CHECK(::write(fd, garbage, sizeof(garbage)) == 17);
    ::close(fd);

    const auto rep = pb2::storage::recover_geoseis_v2_file(file0, true);
    S_CHECK(rep.ok);
    S_CHECK(rep.sample_rate_hz == 200000U);
    S_CHECK(rep.truncated_bytes == 17ULL);
    S_CHECK(rep.valid_frames > 0ULL);

    ::unlink(file0.c_str());
    ::unlink(file1.c_str());
}

void test_geoseis_v2_200k_burst_benchmark() {
    const std::string path = "/tmp/pb2_bench_200k.bin";
    ::unlink(path.c_str());

    pb2::storage::GeoseisWriterConfig cfg{};
    cfg.base_path = path;
    cfg.sample_rate_hz = 200000U;
    cfg.enable_chunking = false;
    cfg.min_free_bytes = 0U;

    pb2::storage::GeoseisV2Writer writer;
    S_CHECK(writer.open(cfg));

    pb2::acquisition::MockSourceConfig mcfg{};
    mcfg.sample_rate_hz = 200000U;
    pb2::acquisition::MockAcquisitionSource src(mcfg);
    std::vector<pb2::acquisition::SampleFrame> batch;
    S_CHECK(src.read_frames(2000, batch));

    const auto t0 = std::chrono::steady_clock::now();
    /* 100 batches * 2000 = 200,000 frames (1.6 MB RAW) */
    for (int i = 0; i < 100; ++i) {
        S_CHECK(writer.write_frames(batch.data(), batch.size()));
    }
    S_CHECK(writer.flush());
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto lat = writer.compute_latency_stats();
    const double mb_s = (static_cast<double>(writer.bytes_written()) / (1024.0 * 1024.0)) / sec;
    std::printf("GEOSEIS1 v2 burst: %llu frames, %.2f MB/s, P50=%.1fus P95=%.1fus P99=%.1fus\n",
                static_cast<unsigned long long>(writer.frames_written()),
                mb_s, lat.p50_us, lat.p95_us, lat.p99_us);

    writer.close();
    const auto rep = pb2::storage::recover_geoseis_v2_file(path, false);
    S_CHECK(rep.ok && rep.valid_frames == 200000ULL && rep.truncated_bytes == 0ULL);
    ::unlink(path.c_str());
}

} // namespace

int main() {
    test_geoseis_v2_layout_gaps_and_recovery();
    test_geoseis_v2_200k_burst_benchmark();
    std::printf("pb2_geoseis_v2_tests: PASS\n");
    return 0;
}
