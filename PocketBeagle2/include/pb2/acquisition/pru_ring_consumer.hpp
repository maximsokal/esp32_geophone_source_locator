#pragma once

#include "pru_adc_shared.h"

#include <cstddef>
#include <vector>

namespace pb2::acquisition {

class PruRingConsumer {
public:
    explicit PruRingConsumer(pb2_pru_ring_t* ring) noexcept;

    bool is_valid() const noexcept;
    std::size_t drain(std::size_t max_frames, std::vector<pb2_pru_sample_frame_t>& out);

private:
    pb2_pru_ring_t* ring_ = nullptr;
};

} // namespace pb2::acquisition
