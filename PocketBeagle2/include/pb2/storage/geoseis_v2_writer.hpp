#pragma once

#include "pb2/acquisition/mock_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pb2::storage {

inline constexpr std::uint16_t kGeoseisVersionV2 = 2U;
inline constexpr std::size_t kGeoseisBaseHeaderBytes = 24U;
inline constexpr std::size_t kGeoseisV2MetadataBytes = 44U;
inline constexpr std::size_t kGeoseisV2BlockHeaderBytes = 20U;
inline constexpr std::uint16_t kGeoseisV2BlockMaxFrames = 256U;
inline constexpr std::uint16_t kGeoseisV2BlockFlagGapBefore = 0x0001U;

struct GeoseisV2Metadata {
    std::array<float, 4> scale_uv_per_lsb{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> offset_uv{0.0f, 0.0f, 0.0f, 0.0f};
    float input_range_v = 5.0f;
    std::uint32_t oversampling_ratio = 0U;
    std::uint32_t spi_clock_hz = 12500000U;
};

struct GeoseisWriterConfig {
    std::string base_path; /* e.g. "/data/session_001" -> writes "_000.bin" or exact path if chunking disabled */
    std::uint32_t sample_rate_hz = 200000U;
    GeoseisV2Metadata metadata{};
    std::uint64_t max_chunk_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL; /* 2 GiB (< 4 GiB FAT32/ext4 limit) */
    std::uint64_t min_free_bytes = 64ULL * 1024ULL * 1024ULL;
    bool enable_chunking = true;
};

struct WriteLatencyStats {
    std::uint64_t block_writes = 0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

struct GeoseisRecoveryReport {
    bool ok = false;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t valid_frames = 0;
    std::uint64_t valid_blocks = 0;
    std::uint64_t gap_blocks = 0;
    std::uint64_t valid_bytes = 0;
    std::uint64_t truncated_bytes = 0;
};

class GeoseisV2Writer {
public:
    GeoseisV2Writer() = default;
    ~GeoseisV2Writer();

    GeoseisV2Writer(const GeoseisV2Writer&) = delete;
    GeoseisV2Writer& operator=(const GeoseisV2Writer&) = delete;

    bool open(const GeoseisWriterConfig& cfg);
    bool write_frames(const pb2::acquisition::SampleFrame* frames, std::size_t count);
    bool flush();
    void close();

    bool is_open() const noexcept { return fd_ >= 0; }
    std::uint64_t frames_written() const noexcept { return total_frames_written_; }
    std::uint64_t bytes_written() const noexcept { return total_bytes_written_; }
    std::uint32_t chunk_index() const noexcept { return chunk_index_; }
    std::uint64_t gap_count() const noexcept { return gap_count_; }
    const std::string& current_path() const noexcept { return current_path_; }
    WriteLatencyStats compute_latency_stats() const;

private:
    bool open_next_chunk();
    bool flush_staging_block();
    bool check_free_space() const;

    GeoseisWriterConfig cfg_{};
    int fd_ = -1;
    std::string current_path_;
    std::uint32_t chunk_index_ = 0;
    std::uint64_t chunk_bytes_ = 0;
    std::uint64_t total_frames_written_ = 0;
    std::uint64_t total_bytes_written_ = 0;
    std::uint64_t gap_count_ = 0;

    bool has_expected_index_ = false;
    std::uint64_t expected_next_index_ = 0;

    std::vector<pb2::acquisition::SampleFrame> staging_;
    bool staging_gap_before_ = false;
    std::vector<double> write_latencies_us_;
};

/// Validates a GEOSEIS1 v2 file, optionally truncating an incomplete trailing block.
GeoseisRecoveryReport recover_geoseis_v2_file(const std::string& path, bool truncate_torn_tail = true);

} // namespace pb2::storage
