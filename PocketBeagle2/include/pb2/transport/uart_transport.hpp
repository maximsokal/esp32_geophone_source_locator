#pragma once

#include "pb2_protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pb2::transport {

struct UartStats {
    std::uint64_t bytes_tx = 0;
    std::uint64_t bytes_rx = 0;
    std::uint64_t frames_tx = 0;
    std::uint64_t frames_rx = 0;
    std::uint64_t duplicate_seq_count = 0;
    std::uint64_t session_rollovers = 0;
    std::uint64_t tx_queue_drops = 0;
    std::uint64_t reconnects = 0;
    std::uint32_t crc_errors = 0;
    std::uint32_t cobs_errors = 0;
    std::uint32_t header_errors = 0;
    std::uint32_t oversized_errors = 0;
};

class UartTransport {
public:
    using FrameCallback = std::function<void(const pb2_frame_header_t&, const uint8_t*)>;

    explicit UartTransport(std::size_t max_tx_queue_frames = 32);
    ~UartTransport();

    UartTransport(const UartTransport&) = delete;
    UartTransport& operator=(const UartTransport&) = delete;

    /// Open a serial TTY or PTY slave device in non-blocking raw mode.
    bool open_device(const std::string& device_path, std::uint32_t baud_rate);

    /// Attach an already-open file descriptor (e.g., PTY master/slave in tests).
    bool attach_fd(int fd, bool take_ownership = true);

    void close_device() noexcept;
    bool is_open() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }

    /// Queue a framed COBS+CRC32C packet without blocking.
    /// Drops oldest/newest per bounded queue and increments tx_queue_drops on saturation.
    bool enqueue_frame(const pb2_frame_header_t& hdr, const uint8_t* payload);

    /// Pump non-blocking TX and RX once (using poll with timeout_ms).
    /// Returns false if peer disconnected (POLLHUP/EOF/fatal error).
    bool poll_once(int timeout_ms, const FrameCallback& on_frame);

    const UartStats& stats() const noexcept { return stats_; }
    std::size_t pending_tx_frames() const noexcept { return tx_queue_.size(); }

private:
    bool flush_tx_nonblocking();
    void handle_decoded_frame(const pb2_frame_header_t& hdr, const uint8_t* payload, const FrameCallback& cb);

    int fd_ = -1;
    bool owns_fd_ = false;
    std::size_t max_tx_queue_frames_ = 32;

    std::deque<std::vector<std::uint8_t>> tx_queue_;
    std::size_t tx_front_offset_ = 0;

    pb2_stream_decoder_t decoder_{};
    UartStats stats_{};

    bool has_peer_epoch_ = false;
    std::uint32_t peer_boot_epoch_ = 0;
    std::uint32_t peer_session_epoch_ = 0;
    std::uint32_t last_rx_sequence_ = 0;
    bool has_rx_sequence_ = false;
};

} // namespace pb2::transport
