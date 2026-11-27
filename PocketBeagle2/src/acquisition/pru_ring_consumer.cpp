#include "pb2/acquisition/pru_ring_consumer.hpp"

#include <atomic>

namespace pb2::acquisition {

PruRingConsumer::PruRingConsumer(pb2_pru_ring_t* ring) noexcept : ring_(ring) {}

bool PruRingConsumer::is_valid() const noexcept {
    return ring_ != nullptr &&
           ring_->magic == PB2_PRU_RING_MAGIC &&
           ring_->version == PB2_PRU_RING_VERSION &&
           ring_->channel_count == PB2_PRU_CHANNEL_COUNT;
}

std::size_t PruRingConsumer::drain(std::size_t max_frames, std::vector<pb2_pru_sample_frame_t>& out) {
    out.clear();
    if (!is_valid()) {
        return 0;
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    std::size_t count = 0;
    while (count < max_frames && ring_->read_idx != ring_->write_idx) {
        const auto& slot = ring_->slots[ring_->read_idx];
        pb2_pru_sample_frame_t copy{};
        copy.source_sample_index = slot.source_sample_index;
        copy.pru_tick_counter = slot.pru_tick_counter;
        copy.sequence = slot.sequence;
        copy.flags = slot.flags;
        for (std::size_t ch = 0; ch < PB2_PRU_CHANNEL_COUNT; ++ch) {
            copy.channels[ch] = slot.channels[ch];
        }
        out.push_back(copy);
        ring_->read_idx = (ring_->read_idx + 1U) & (PB2_PRU_RING_CAPACITY - 1U);
        ++count;
    }
    std::atomic_thread_fence(std::memory_order_release);
    return count;
}

} // namespace pb2::acquisition
