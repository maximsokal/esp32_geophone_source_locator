#include "pb2/acquisition/pru_ring_consumer.hpp"
#include "pb2/hardware/pinmap_a0.hpp"
#include "pb2/power/shutdown_fsm.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    /* Single DOUT @ 100 kSPS: 64 / 12.5 MHz = 5.12 us > 3.45 us BUSY high window */
    const auto single_100k = pb2::hardware::evaluate_ad7606_timing(
        pb2::acquisition::AdcWiringProfile::SerialSingleDout, 100000U, 12.5);
    CHECK(!single_100k.feasible);

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

void test_pru_ring_consumer() {
    pb2_pru_ring_t ring{};
    ring.magic = PB2_PRU_RING_MAGIC;
    ring.version = PB2_PRU_RING_VERSION;
    ring.channel_count = PB2_PRU_CHANNEL_COUNT;
    ring.profile = PB2_PRU_PROFILE_DUAL_DOUT;
    ring.sample_rate_hz = 200000U;

    for (std::uint32_t i = 0; i < 4; ++i) {
        auto& s = ring.slots[i];
        s.source_sample_index = 1000ULL + i;
        s.pru_tick_counter = i * 1000ULL;
        s.sequence = i;
        s.channels[0] = static_cast<std::int16_t>(100 + i);
        s.channels[1] = static_cast<std::int16_t>(-200 - i);
        s.channels[2] = static_cast<std::int16_t>(300 + i);
        s.channels[3] = static_cast<std::int16_t>(-400 - i);
        ring.write_idx = i + 1U;
    }

    pb2::acquisition::PruRingConsumer consumer(&ring);
    CHECK(consumer.is_valid());
    std::vector<pb2_pru_sample_frame_t> out;
    CHECK(consumer.drain(16, out) == 4U);
    CHECK(out[0].source_sample_index == 1000ULL);
    CHECK(out[3].channels[3] == -403);
    CHECK(ring.read_idx == ring.write_idx);
}

} // namespace

int main() {
    test_pinmap_and_ad7606_timing();
    test_shutdown_fsm();
    test_pru_ring_consumer();
    std::printf("pb2_phase3_4_tests: PASS\n");
    return 0;
}
