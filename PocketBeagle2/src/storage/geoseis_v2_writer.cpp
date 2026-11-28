#include "pb2/storage/geoseis_v2_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace pb2::storage {

namespace {

void put_u16_le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
}

void put_i16_le(std::uint8_t* p, std::int16_t v) {
    put_u16_le(p, static_cast<std::uint16_t>(v));
}

void put_u32_le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
}

void put_u64_le(std::uint8_t* p, std::uint64_t v) {
    put_u32_le(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    put_u32_le(p + 4, static_cast<std::uint32_t>((v >> 32U) & 0xFFFFFFFFULL));
}

void put_f32_le(std::uint8_t* p, float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    put_u32_le(p, u);
}

std::uint16_t get_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t get_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

bool write_all(int fd, const std::uint8_t* buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, buf + off, len - off);
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string format_chunk_path(const std::string& base, std::uint32_t idx, bool chunking) {
    if (!chunking) {
        return base;
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%03u.bin", static_cast<unsigned>(idx));
    return base + suffix;
}

} // namespace

GeoseisV2Writer::~GeoseisV2Writer() {
    close();
}

bool GeoseisV2Writer::check_free_space() const {
    if (cfg_.min_free_bytes == 0) {
        return true;
    }
    const auto slash = cfg_.base_path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? "." : cfg_.base_path.substr(0, slash);
    struct statvfs vfs {};
    if (::statvfs(dir.empty() ? "." : dir.c_str(), &vfs) != 0) {
        return true; /* If statvfs unavailable, don't spuriously fail */
    }
    const std::uint64_t avail = static_cast<std::uint64_t>(vfs.f_bavail) *
                                static_cast<std::uint64_t>(vfs.f_frsize);
    return avail >= cfg_.min_free_bytes;
}

bool GeoseisV2Writer::open(const GeoseisWriterConfig& cfg) {
    close();
    if (cfg.base_path.empty() || cfg.sample_rate_hz == 0U) {
        return false;
    }
    cfg_ = cfg;
    chunk_index_ = 0;
    total_frames_written_ = 0;
    total_bytes_written_ = 0;
    gap_count_ = 0;
    has_expected_index_ = false;
    expected_next_index_ = 0;
    staging_.clear();
    staging_.reserve(kGeoseisV2BlockMaxFrames);
    staging_gap_before_ = false;
    write_latencies_us_.clear();
    latency_write_cursor_ = 0U;

    if (!check_free_space()) {
        return false;
    }
    return open_next_chunk();
}

bool GeoseisV2Writer::open_next_chunk() {
    if (fd_ >= 0) {
        const bool synced = (::fsync(fd_) == 0);
        ::close(fd_);
        fd_ = -1;
        if (!synced) {
            return false;
        }
    }

    current_path_ = format_chunk_path(cfg_.base_path, chunk_index_, cfg_.enable_chunking);
    const int open_flags = O_WRONLY | O_CREAT | (cfg_.allow_overwrite ? O_TRUNC : O_EXCL);
    fd_ = ::open(current_path_.c_str(), open_flags, 0644);
    if (fd_ < 0) {
        return false;
    }

    std::uint8_t hdr[kGeoseisBaseHeaderBytes + kGeoseisV2MetadataBytes]{};
    std::memcpy(hdr, "GEOSEIS1", 8);
    put_u16_le(hdr + 8, kGeoseisVersionV2);
    put_u16_le(hdr + 10, 4U);
    put_u32_le(hdr + 12, cfg_.sample_rate_hz);
    put_u32_le(hdr + 16, 8U); /* 4 * int16 = 8 bytes */
    put_u32_le(hdr + 20, 0U);

    std::size_t off = kGeoseisBaseHeaderBytes;
    for (std::size_t i = 0; i < 4; ++i) {
        put_f32_le(hdr + off, cfg_.metadata.scale_uv_per_lsb[i]);
        off += 4;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        put_f32_le(hdr + off, cfg_.metadata.offset_uv[i]);
        off += 4;
    }
    put_f32_le(hdr + off, cfg_.metadata.input_range_v);
    put_u32_le(hdr + off + 4, cfg_.metadata.oversampling_ratio);
    put_u32_le(hdr + off + 8, cfg_.metadata.spi_clock_hz);

    if (!write_all(fd_, hdr, sizeof(hdr))) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    chunk_bytes_ = sizeof(hdr);
    total_bytes_written_ += sizeof(hdr);
    return true;
}

bool GeoseisV2Writer::flush_staging_block() {
    if (staging_.empty()) {
        return true;
    }
    if (fd_ < 0) {
        return false;
    }

    const std::size_t block_bytes = kGeoseisV2BlockHeaderBytes + staging_.size() * 8U;
    if (cfg_.enable_chunking && cfg_.max_chunk_bytes > 0 &&
        (chunk_bytes_ + block_bytes > cfg_.max_chunk_bytes)) {
        ++chunk_index_;
        if (!open_next_chunk()) {
            return false;
        }
    }

    std::uint8_t buf[kGeoseisV2BlockHeaderBytes + kGeoseisV2BlockMaxFrames * 8U]{};
    put_u64_le(buf, staging_.front().source_sample_index);
    put_u64_le(buf + 8, staging_.front().timestamp_us);
    put_u16_le(buf + 16, static_cast<std::uint16_t>(staging_.size()));
    put_u16_le(buf + 18, staging_gap_before_ ? kGeoseisV2BlockFlagGapBefore : 0U);

    std::size_t off = kGeoseisV2BlockHeaderBytes;
    for (const auto& f : staging_) {
        for (std::size_t ch = 0; ch < 4; ++ch) {
            put_i16_le(buf + off, f.channels[ch]);
            off += 2;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = write_all(fd_, buf, block_bytes);
    const auto dt = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
        return false;
    }

    if (write_latencies_us_.size() < kMaxLatencySamples) {
        write_latencies_us_.push_back(dt);
    } else {
        write_latencies_us_[latency_write_cursor_] = dt;
        latency_write_cursor_ = (latency_write_cursor_ + 1U) % kMaxLatencySamples;
    }
    chunk_bytes_ += block_bytes;
    total_bytes_written_ += block_bytes;
    total_frames_written_ += staging_.size();
    staging_.clear();
    staging_gap_before_ = false;
    return true;
}

bool GeoseisV2Writer::write_frames(const pb2::acquisition::SampleFrame* frames, std::size_t count) {
    if (fd_ < 0 || (frames == nullptr && count > 0)) {
        return false;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto& f = frames[i];
        const bool gap = has_expected_index_ && (f.source_sample_index != expected_next_index_);
        if (gap) {
            ++gap_count_;
            if (!staging_.empty() && !flush_staging_block()) {
                return false;
            }
            staging_gap_before_ = true;
        }

        staging_.push_back(f);
        has_expected_index_ = true;
        expected_next_index_ = f.source_sample_index + 1ULL;

        if (staging_.size() >= kGeoseisV2BlockMaxFrames) {
            if (!flush_staging_block()) {
                return false;
            }
        }
    }
    return true;
}

bool GeoseisV2Writer::flush() {
    if (!flush_staging_block()) {
        return false;
    }
    if (fd_ >= 0) {
        return ::fsync(fd_) == 0;
    }
    return true;
}

void GeoseisV2Writer::close() {
    if (fd_ >= 0) {
        (void)flush();
        ::close(fd_);
        fd_ = -1;
    }
}

WriteLatencyStats GeoseisV2Writer::compute_latency_stats() const {
    WriteLatencyStats st{};
    st.block_writes = write_latencies_us_.size();
    if (write_latencies_us_.empty()) {
        return st;
    }
    std::vector<double> sorted = write_latencies_us_;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    };
    st.p50_us = pct(0.50);
    st.p95_us = pct(0.95);
    st.p99_us = pct(0.99);
    st.max_us = sorted.back();
    return st;
}

GeoseisRecoveryReport recover_geoseis_v2_file(const std::string& path, bool truncate_torn_tail) {
    GeoseisRecoveryReport rep{};
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return rep;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return rep;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
    constexpr std::uint64_t kHeaderTotal = kGeoseisBaseHeaderBytes + kGeoseisV2MetadataBytes;
    if (file_size < kHeaderTotal) {
        ::close(fd);
        return rep;
    }

    std::uint8_t hdr[kHeaderTotal]{};
    if (::pread(fd, hdr, sizeof(hdr), 0) != static_cast<ssize_t>(sizeof(hdr))) {
        ::close(fd);
        return rep;
    }
    if (std::memcmp(hdr, "GEOSEIS1", 8) != 0 ||
        get_u16_le(hdr + 8) != kGeoseisVersionV2 ||
        get_u16_le(hdr + 10) != 4U ||
        get_u32_le(hdr + 16) != 8U) {
        ::close(fd);
        return rep;
    }
    rep.sample_rate_hz = get_u32_le(hdr + 12);

    std::uint64_t off = kHeaderTotal;
    while (off + kGeoseisV2BlockHeaderBytes <= file_size) {
        std::uint8_t bhdr[kGeoseisV2BlockHeaderBytes]{};
        if (::pread(fd, bhdr, sizeof(bhdr), static_cast<off_t>(off)) !=
            static_cast<ssize_t>(sizeof(bhdr))) {
            break;
        }
        const std::uint16_t fc = get_u16_le(bhdr + 16);
        const std::uint16_t flags = get_u16_le(bhdr + 18);
        if (fc == 0U || fc > kGeoseisV2BlockMaxFrames) {
            break;
        }
        const std::uint64_t block_len = kGeoseisV2BlockHeaderBytes + static_cast<std::uint64_t>(fc) * 8ULL;
        if (off + block_len > file_size) {
            break;
        }
        off += block_len;
        ++rep.valid_blocks;
        rep.valid_frames += fc;
        if (flags & kGeoseisV2BlockFlagGapBefore) {
            ++rep.gap_blocks;
        }
    }

    rep.valid_bytes = off;
    rep.truncated_bytes = file_size - off;
    if (truncate_torn_tail && rep.truncated_bytes > 0) {
        if (::ftruncate(fd, static_cast<off_t>(off)) != 0) {
            ::close(fd);
            return rep;
        }
    }
    ::close(fd);
    rep.ok = true;
    return rep;
}

} // namespace pb2::storage
