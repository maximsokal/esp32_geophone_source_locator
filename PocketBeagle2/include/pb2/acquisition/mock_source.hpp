#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pb2::acquisition {

inline constexpr std::size_t kChannelCount = 4;

enum class AdcWiringProfile : std::uint8_t {
    SerialSingleDout = 0, // Geophones 1-4 -> V1-V4 on DOUTA (64 SCLK)
    SerialDualDout = 1    // Geophones 1-2 -> V1-V2 (DOUTA), 3-4 -> V5-V6 (DOUTB) (32 SCLK)
};

struct SampleFrame {
    std::uint64_t source_sample_index = 0;
    std::uint64_t timestamp_us = 0;
    std::array<std::int16_t, kChannelCount> channels{};
    std::uint32_t flags = 0;
};

struct MockSourceConfig {
    std::uint32_t sample_rate_hz = 500;
    AdcWiringProfile profile = AdcWiringProfile::SerialDualDout;
    std::uint64_t start_sample_index = 0;
    std::uint64_t start_timestamp_us = 0;
    std::uint32_t inject_drop_every_n_frames = 0;
};

struct MockSourceStats {
    std::uint64_t generated_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t last_sample_index = 0;
    std::uint64_t last_timestamp_us = 0;
};

class MockAcquisitionSource {
public:
    explicit MockAcquisitionSource(const MockSourceConfig& config = {});

    bool is_valid() const noexcept;
    const MockSourceConfig& config() const noexcept { return config_; }
    const MockSourceStats& stats() const noexcept { return stats_; }

    /// Logical geophone order [1..4] mapped to physical AD7606 inputs [V1..V8].
    std::array<std::uint8_t, kChannelCount> physical_channel_map() const noexcept;

    /// Generates up to `count` deterministic 4-channel int16 frames.
    /// Returns false if configuration is invalid.
    bool read_frames(std::size_t count, std::vector<SampleFrame>& out_frames);

    void reset() noexcept;

private:
    MockSourceConfig config_{};
    MockSourceStats stats_{};
    std::uint64_t next_sample_index_ = 0;
    bool valid_ = false;
};

} // namespace pb2::acquisition
