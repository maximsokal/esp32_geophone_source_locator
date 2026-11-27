#include "pb2/transport/uart_transport.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace pb2::transport {

namespace {

speed_t to_termios_baud(std::uint32_t baud) {
    switch (baud) {
        case 115200U: return B115200;
        case 460800U: return B460800;
        case 921600U: return B921600;
        default:      return B115200;
    }
}

bool configure_raw_nonblocking(int fd, std::uint32_t baud_rate) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    if (::isatty(fd) == 1) {
        struct termios tio {};
        if (::tcgetattr(fd, &tio) != 0) {
            return false;
        }
        ::cfmakeraw(&tio);
        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= ~CRTSCTS;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        const speed_t spd = to_termios_baud(baud_rate);
        ::cfsetispeed(&tio, spd);
        ::cfsetospeed(&tio, spd);
        if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

UartTransport::UartTransport(std::size_t max_tx_queue_frames)
    : max_tx_queue_frames_(max_tx_queue_frames > 0 ? max_tx_queue_frames : 1) {
    pb2_stream_decoder_init(&decoder_);
}

UartTransport::~UartTransport() {
    close_device();
}

bool UartTransport::open_device(const std::string& device_path, std::uint32_t baud_rate) {
    close_device();
    const int fd = ::open(device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_raw_nonblocking(fd, baud_rate)) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    owns_fd_ = true;
    pb2_stream_decoder_init(&decoder_);
    return true;
}

bool UartTransport::attach_fd(int fd, bool take_ownership) {
    close_device();
    if (fd < 0 || !configure_raw_nonblocking(fd, 115200U)) {
        return false;
    }
    fd_ = fd;
    owns_fd_ = take_ownership;
    pb2_stream_decoder_init(&decoder_);
    return true;
}

void UartTransport::close_device() noexcept {
    if (fd_ >= 0 && owns_fd_) {
        ::close(fd_);
    }
    fd_ = -1;
    owns_fd_ = false;
    tx_queue_.clear();
    tx_front_offset_ = 0;
}

bool UartTransport::enqueue_frame(const pb2_frame_header_t& hdr, const uint8_t* payload) {
    std::vector<std::uint8_t> wire(PB2_PROTO_MAX_COBS_BYTES);
    std::size_t wire_len = 0;
    if (pb2_wire_encode(&hdr, payload, wire.data(), wire.size(), &wire_len) != PB2_PROTO_OK) {
        return false;
    }
    wire.resize(wire_len);

    if (tx_queue_.size() >= max_tx_queue_frames_) {
        ++stats_.tx_queue_drops;
        return false;
    }
    tx_queue_.push_back(std::move(wire));
    return true;
}

bool UartTransport::flush_tx_nonblocking() {
    while (!tx_queue_.empty()) {
        const auto& front = tx_queue_.front();
        const std::size_t remaining = front.size() - tx_front_offset_;
        const ssize_t n = ::write(fd_, front.data() + tx_front_offset_, remaining);
        if (n > 0) {
            stats_.bytes_tx += static_cast<std::uint64_t>(n);
            tx_front_offset_ += static_cast<std::size_t>(n);
            if (tx_front_offset_ >= front.size()) {
                tx_queue_.pop_front();
                tx_front_offset_ = 0;
                ++stats_.frames_tx;
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return false;
        }
    }
    return true;
}

void UartTransport::handle_decoded_frame(
    const pb2_frame_header_t& hdr,
    const uint8_t* payload,
    const FrameCallback& cb
) {
    ++stats_.frames_rx;

    if (!has_peer_epoch_ ||
        hdr.boot_epoch != peer_boot_epoch_ ||
        hdr.session_epoch != peer_session_epoch_) {
        if (has_peer_epoch_) {
            ++stats_.session_rollovers;
        }
        has_peer_epoch_ = true;
        peer_boot_epoch_ = hdr.boot_epoch;
        peer_session_epoch_ = hdr.session_epoch;
        has_rx_sequence_ = false;
    }

    if (has_rx_sequence_ && hdr.sequence == last_rx_sequence_) {
        ++stats_.duplicate_seq_count;
    }
    last_rx_sequence_ = hdr.sequence;
    has_rx_sequence_ = true;

    if (cb) {
        cb(hdr, payload);
    }
}

bool UartTransport::poll_once(int timeout_ms, const FrameCallback& on_frame) {
    if (fd_ < 0) {
        return false;
    }

    struct pollfd pfd {};
    pfd.fd = fd_;
    pfd.events = POLLIN | (!tx_queue_.empty() ? POLLOUT : 0);

    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        return errno == EINTR;
    }

    if (pfd.revents & (POLLERR | POLLNVAL)) {
        return false;
    }

    if (pfd.revents & POLLOUT) {
        if (!flush_tx_nonblocking()) {
            return false;
        }
    }

    if (pfd.revents & POLLIN) {
        std::uint8_t buf[512];
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                stats_.bytes_rx += static_cast<std::uint64_t>(n);
                struct CbBridge {
                    UartTransport* self;
                    const FrameCallback* cb;
                } bridge{this, &on_frame};

                pb2_stream_decoder_feed(
                    &decoder_,
                    buf,
                    static_cast<std::size_t>(n),
                    [](const pb2_frame_header_t* hdr, const uint8_t* payload, void* ctx) {
                        auto* b = static_cast<CbBridge*>(ctx);
                        b->self->handle_decoded_frame(*hdr, payload, *b->cb);
                    },
                    &bridge
                );
                stats_.crc_errors = decoder_.crc_errors;
                stats_.cobs_errors = decoder_.cobs_errors;
                stats_.header_errors = decoder_.header_errors;
                stats_.oversized_errors = decoder_.oversized_errors;
            } else if (n == 0) {
                return false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
        }
    }

    if (pfd.revents & POLLHUP) {
        return false;
    }

    return true;
}

} // namespace pb2::transport
