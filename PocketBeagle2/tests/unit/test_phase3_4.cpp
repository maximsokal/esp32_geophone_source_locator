#include "pb2/acquisition/pru_ring_consumer.hpp"
#include "pb2/hardware/pinmap_a0.hpp"
#include "pb2/power/shutdown_fsm.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Check failed: %s (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                               \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

void test_pinmap_and_ad7606_timing() {
    CHECK(pb2::hardware::verify_a0_pinmap_conflict_free());

    /* Single DOUT (64 SCLK @ 12.5 MHz = 5.12 us) CANNOT reach 200 kSPS */
    const auto single_200k = pb2::hardware::evaluate_ad7606_timing(
        pb2::acquisition::AdcWiringProfile::SerialSingleDout, 200000U, 12.5);
    CHECK(!single_200k.feasible);

    /* Single DOUT @ 20 kSPS (50 us period) IS feasible via Read After Conversion (Fig. 2) */
    const auto single_20k = pb2::hardware::evaluate_ad7606_timing(
        pb2::acquisition::AdcWiringProfile::SerialSingleDout, 20000U, 12.5);
    CHECK(single_20k.feasible);
    CHECK(single_20k.sclk_cycles == 64U);

    /* Dual DOUT (32 SCLK @ 12.5 MHz = 2.56 us) DOES fit 200 kSPS with >25 ns margin */
    const auto dual_200k = pb2::hardware::evaluate_ad7606_timing(
        pb2::acquisition::AdcWiringProfile::SerialDualDout, 200000U, 12.5);
    CHECK(dual_200k.feasible);
    CHECK(dual_200k.sclk_cycles == 32U);
    CHECK(dual_200k.margin_before_busy_fall_ns >= 25.0);

    /* Reject >12.5 MHz SCLK at VDRIVE = 3.3 V */
    const auto over_sclk = pb2::hardware::evaluate_ad7606_timing(
        pb2::acquisition::AdcWiringProfile::SerialDualDout, 200000U, 15.0);
    CHECK(!over_sclk.feasible);
}

void test_shutdown_fsm() {
    pb2::power::ShutdownFsm fsm;
    CHECK(fsm.state() == pb2::power::ShutdownState::Running);
    CHECK(!fsm.is_safe_to_cut_power());

    CHECK(fsm.on_event(pb2::power::ShutdownEvent::RequestStop));
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::AcquisitionStopped));
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::StorageFlushed));
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::SessionFinalized));
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::PruStopped));
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::FilesystemSynced));
    CHECK(!fsm.is_safe_to_cut_power()); /* Kernel not halted yet */
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::KernelHalted));
    CHECK(fsm.is_safe_to_cut_power());

    fsm.reset();
    CHECK(fsm.on_event(pb2::power::ShutdownEvent::TimeoutOrError));
    CHECK(fsm.is_faulted());
    CHECK(!fsm.is_safe_to_cut_power());
}

void init_shm(pb2_pru_shared_t& shm) {
    std::memset(&shm, 0, sizeof(shm));
    shm.magic = PB2_PRU_RING_MAGIC;
    shm.version = PB2_PRU_RING_VERSION;
    shm.channel_count = PB2_PRU_CHANNEL_COUNT;
    shm.profile = PB2_PRU_PROFILE_DUAL_DOUT;
    shm.sample_rate_hz = 200000U;
    shm.iep_clock_hz = 200000000U;
}

void test_pru_ring_consumer_blocks_and_wraparound() {
    auto shm = std::make_unique<pb2_pru_shared_t>();
    init_shm(*shm);

    pb2::acquisition::PruSharedMemory mem;
    CHECK(mem.attach_external(shm.get()));
    pb2::acquisition::PruRingConsumer consumer(mem.get());
    CHECK(consumer.is_valid());

    // Push 100 blocks (> 64 ring capacity) in batches to test wraparound & gap flag
    constexpr std::uint32_t kTotalBlocks = 100U;
    std::uint64_t next_sample = 0;
    std::vector<pb2::acquisition::SampleFrame> all_frames;

    for (std::uint32_t b = 0; b < kTotalBlocks; ++b) {
        const std::uint32_t w = shm->write_block_idx;
        auto& blk = shm->blocks[w];
        blk.first_sample_index = next_sample;
        blk.first_iep_ticks = next_sample * 1000ULL;
        blk.sequence = b;
        blk.frame_count = PB2_PRU_BLOCK_FRAMES;
        blk.flags = (b == 10U) ? PB2_PRU_BLK_FLAG_GAP_BEFORE : 0U;
        for (std::uint16_t f = 0; f < PB2_PRU_BLOCK_FRAMES; ++f) {
            const std::int16_t base = static_cast<std::int16_t>((b * 16U + f) & 0x3FFFU);
            blk.channels[f][0] = base;
            blk.channels[f][1] = static_cast<std::int16_t>(-base);
            blk.channels[f][2] = static_cast<std::int16_t>(base + 10);
            blk.channels[f][3] = static_cast<std::int16_t>(-base - 10);
        }
        next_sample += PB2_PRU_BLOCK_FRAMES;
        shm->write_block_idx = (w + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U);

        // Drain every 4 blocks so ring advances and wraps past 64
        if ((b % 4U) == 3U) {
            const auto drained = consumer.drain(0, all_frames);
            CHECK(drained == 4U * PB2_PRU_BLOCK_FRAMES);
        }
    }

    CHECK(all_frames.size() == kTotalBlocks * PB2_PRU_BLOCK_FRAMES);
    CHECK(all_frames[0].source_sample_index == 0ULL);
    CHECK(all_frames[160].flags == PB2_PRU_BLK_FLAG_GAP_BEFORE);
    CHECK(all_frames.back().source_sample_index == kTotalBlocks * PB2_PRU_BLOCK_FRAMES - 1U);
    CHECK(consumer.snapshot_stats().gap_blocks == 1U);
    CHECK(shm->read_block_idx == shm->write_block_idx);
}

void test_pru_ring_concurrent_producer_consumer_and_file_backend() {
    auto shm = std::make_unique<pb2_pru_shared_t>();
    init_shm(*shm);

    constexpr std::uint32_t kBlocks = 500U;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (std::uint32_t b = 0; b < kBlocks; ++b) {
            for (;;) {
                const std::uint32_t w = __atomic_load_n(&shm->write_block_idx, __ATOMIC_RELAXED);
                const std::uint32_t next_w = (w + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U);
                const std::uint32_t r = __atomic_load_n(&shm->read_block_idx, __ATOMIC_ACQUIRE);
                if (next_w != r) {
                    auto& blk = shm->blocks[w];
                    blk.first_sample_index = static_cast<std::uint64_t>(b) * PB2_PRU_BLOCK_FRAMES;
                    blk.first_iep_ticks = blk.first_sample_index * 1000ULL;
                    blk.sequence = b;
                    blk.frame_count = PB2_PRU_BLOCK_FRAMES;
                    blk.flags = 0U;
                    for (std::uint16_t f = 0; f < PB2_PRU_BLOCK_FRAMES; ++f) {
                        blk.channels[f][0] = static_cast<std::int16_t>((b + f) & 0x7FFFU);
                    }
                    __atomic_store_n(&shm->write_block_idx, next_w, __ATOMIC_RELEASE);
                    break;
                }
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    pb2::acquisition::PruRingConsumer consumer(shm.get());
    std::vector<pb2::acquisition::SampleFrame> collected;
    while (!producer_done.load(std::memory_order_acquire) ||
           __atomic_load_n(&shm->read_block_idx, __ATOMIC_RELAXED) !=
               __atomic_load_n(&shm->write_block_idx, __ATOMIC_ACQUIRE)) {
        consumer.drain(8, collected);
    }
    producer.join();

    CHECK(collected.size() == kBlocks * PB2_PRU_BLOCK_FRAMES);
    for (std::size_t i = 0; i < collected.size(); ++i) {
        CHECK(collected[i].source_sample_index == i);
    }

    // Verify file-backed MemoryBackend (used to consume PRU simulator binary dumps)
    const char* tmp_path = "/tmp/pb2_pru_shm_test.bin";
    std::FILE* fp = std::fopen(tmp_path, "wb");
    CHECK(fp != nullptr);
    CHECK(std::fwrite(shm.get(), sizeof(pb2_pru_shared_t), 1, fp) == 1);
    std::fclose(fp);

    pb2::acquisition::PruSharedMemory file_mem;
    CHECK(file_mem.open_file(tmp_path, false));
    pb2::acquisition::PruRingConsumer file_consumer(file_mem.get());
    CHECK(file_consumer.is_valid());
    file_mem.close();
    std::remove(tmp_path);
}

} // namespace

int main() {
    test_pinmap_and_ad7606_timing();
    test_shutdown_fsm();
    test_pru_ring_consumer_blocks_and_wraparound();
    test_pru_ring_concurrent_producer_consumer_and_file_backend();
    std::printf("pb2_phase3_4_tests: ALL PASSED\n");
    return 0;
}
