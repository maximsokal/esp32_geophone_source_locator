#include "pb2/acquisition/mock_source.hpp"

#include <cmath>

namespace pb2::acquisition {

namespace {
constexpr std::uint32_t kMaxSupportedSampleRateHz = 200000U;
constexpr double kTwoPi = 6.28318530717958647692;
} // namespace

MockAcquisitionSource::MockAcquisitionSource(const MockSourceConfig& config)
    : config_(config),
      next_sample_index_(config.start_sample_index) {
    valid_ = (config_.sample_rate_hz > 0U && config_.sample_rate_hz <= kMaxSupportedSampleRateHz);
}

bool MockAcquisitionSource::is_valid() const noexcept {
    return valid_;
}

std::array<std::uint8_t, kChannelCount> MockAcquisitionSource::physical_channel_map() const noexcept {
    if (config_.profile == AdcWiringProfile::SerialDualDout) {
        return {1U, 2U, 5U, 6U};
    }
    return {1U, 2U, 3U, 4U};
}

bool MockAcquisitionSource::read_frames(std::size_t count, std::vector<SampleFrame>& out_frames) {
    out_frames.clear();
    if (!valid_) {
        return false;
    }
    out_frames.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        if (config_.inject_drop_every_n_frames > 0U &&
            ((next_sample_index_ + 1ULL) % config_.inject_drop_every_n_frames) == 0ULL) {
            ++next_sample_index_;
            ++stats_.dropped_frames;
        }

        SampleFrame frame{};
        frame.source_sample_index = next_sample_index_;
        const std::uint64_t delta_samples = next_sample_index_ - config_.start_sample_index;
        frame.timestamp_us = config_.start_timestamp_us +
                             (delta_samples * 1000000ULL) / config_.sample_rate_hz;

        const double t_sec = static_cast<double>(delta_samples) / static_cast<double>(config_.sample_rate_hz);
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            const double freq_hz = 20.0 + 15.0 * static_cast<double>(ch);
            const double amplitude = 2000.0 + 500.0 * static_cast<double>(ch);
            const double value = std::sin(kTwoPi * freq_hz * t_sec) * amplitude;
            frame.channels[ch] = static_cast<std::int16_t>(std::llround(value));
        }

        out_frames.push_back(frame);
        stats_.last_sample_index = frame.source_sample_index;
        stats_.last_timestamp_us = frame.timestamp_us;
        ++stats_.generated_frames;
        ++next_sample_index_;
    }

    return true;
}

void MockAcquisitionSource::reset() noexcept {
    stats_ = {};
    next_sample_index_ = config_.start_sample_index;
}

} // namespace pb2::acquisition
