#include "pb2/transport/uart_transport.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#define PTY_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "PTY check failed: %s (%s:%d)\n", #cond,   \
                         __FILE__, __LINE__);                               \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

struct PtyPair {
    int master = -1;
    int slave = -1;
    char slave_name[128]{};

    PtyPair() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        PTY_CHECK(master >= 0);
        PTY_CHECK(::grantpt(master) == 0);
        PTY_CHECK(::unlockpt(master) == 0);
        const char* name = ::ptsname(master);
        PTY_CHECK(name != nullptr);
        std::snprintf(slave_name, sizeof(slave_name), "%s", name);
        slave = ::open(slave_name, O_RDWR | O_NOCTTY);
        PTY_CHECK(slave >= 0);
        struct termios tio {};
        ::cfmakeraw(&tio);
        ::tcsetattr(slave, TCSANOW, &tio);
    }
    ~PtyPair() {
        if (master >= 0) ::close(master);
        if (slave >= 0) ::close(slave);
    }
};

void pump_both(pb2::transport::UartTransport& a,
               pb2::transport::UartTransport& b,
               std::vector<pb2_frame_header_t>& rx_a,
               std::vector<pb2_frame_header_t>& rx_b,
               int rounds = 10) {
    for (int i = 0; i < rounds; ++i) {
        a.poll_once(5, [&](const pb2_frame_header_t& h, const uint8_t*) { rx_a.push_back(h); });
        b.poll_once(5, [&](const pb2_frame_header_t& h, const uint8_t*) { rx_b.push_back(h); });
    }
}

void test_pty_fragmented_multiframe_and_resync() {
    PtyPair pty;
    pb2::transport::UartTransport dev;
    PTY_CHECK(dev.open_device(pty.slave_name, 115200U));

    pb2::transport::UartTransport peer;
    PTY_CHECK(peer.attach_fd(pty.master, false));

    /* 1. Multi-frame per read + wraparound sequence + session rollover */
    pb2_hello_payload_t hello{1U, 0x11U, PB2_PROTO_MAX_PAYLOAD, 0x03U};
    std::uint8_t payload[16]{};
    std::uint16_t plen = 0;
    PTY_CHECK(pb2_encode_hello(&hello, payload, sizeof(payload), &plen) == PB2_PROTO_OK);

    pb2_frame_header_t h1{PB2_PROTO_VERSION, PB2_MSG_HELLO, 0U, 0xFFFFFFFFU, 1U, 1U, plen};
    pb2_frame_header_t h2{PB2_PROTO_VERSION, PB2_MSG_HELLO, 0U, 0x00000000U, 1U, 1U, plen};
    pb2_frame_header_t h2_dup = h2;
    pb2_frame_header_t h3{PB2_PROTO_VERSION, PB2_MSG_HELLO, 0U, 0x00000001U, 1U, 2U, plen};

    PTY_CHECK(peer.enqueue_frame(h1, payload));
    PTY_CHECK(peer.enqueue_frame(h2, payload));
    PTY_CHECK(peer.enqueue_frame(h2_dup, payload));
    PTY_CHECK(peer.enqueue_frame(h3, payload));

    std::vector<pb2_frame_header_t> rx_dev, rx_peer;
    pump_both(dev, peer, rx_dev, rx_peer, 12);

    PTY_CHECK(rx_dev.size() == 4U);
    PTY_CHECK(rx_dev[0].sequence == 0xFFFFFFFFU);
    PTY_CHECK(rx_dev[1].sequence == 0U);
    PTY_CHECK(dev.stats().duplicate_seq_count == 1U);
    PTY_CHECK(dev.stats().session_rollovers == 1U);

    /* 2. Inject corrupted CRC + truncated + oversized + unknown version, then resync */
    std::uint8_t wire[PB2_PROTO_MAX_COBS_BYTES]{};
    std::size_t wire_len = 0;
    PTY_CHECK(pb2_wire_encode(&h1, payload, wire, sizeof(wire), &wire_len) == PB2_PROTO_OK);

    std::uint8_t bad_crc[PB2_PROTO_MAX_COBS_BYTES]{};
    std::memcpy(bad_crc, wire, wire_len);
    bad_crc[wire_len - 3] ^= 0xFFU; /* corrupt payload/CRC byte */
    PTY_CHECK(::write(pty.master, bad_crc, wire_len) == static_cast<ssize_t>(wire_len));

    /* Truncated garbage terminated by 0x00 */
    const std::uint8_t trunc[] = {0x02U, 0x50U, 0x00U};
    PTY_CHECK(::write(pty.master, trunc, sizeof(trunc)) == static_cast<ssize_t>(sizeof(trunc)));

    /* Oversized stream (> PB2_PROTO_MAX_COBS_BYTES) then 0x00 */
    std::vector<std::uint8_t> huge(PB2_PROTO_MAX_COBS_BYTES + 16U, 0x42U);
    huge.push_back(0x00U);
    PTY_CHECK(::write(pty.master, huge.data(), huge.size()) == static_cast<ssize_t>(huge.size()));

    /* Fragmented valid frame (2 chunks) after errors -> must resync cleanly */
    const std::size_t split = wire_len / 2U;
    PTY_CHECK(::write(pty.master, wire, split) == static_cast<ssize_t>(split));
    dev.poll_once(10, [&](const pb2_frame_header_t& h, const uint8_t*) { rx_dev.push_back(h); });
    PTY_CHECK(::write(pty.master, wire + split, wire_len - split) == static_cast<ssize_t>(wire_len - split));
    dev.poll_once(10, [&](const pb2_frame_header_t& h, const uint8_t*) { rx_dev.push_back(h); });

    PTY_CHECK(rx_dev.size() == 5U);
    PTY_CHECK(dev.stats().crc_errors == 1U);
    PTY_CHECK(dev.stats().header_errors >= 1U);
    PTY_CHECK(dev.stats().oversized_errors >= 1U);
}

void test_pty_backpressure_disconnect_and_throughput() {
    PtyPair pty;
    pb2::transport::UartTransport tx_node(4); /* small queue of 4 frames */
    PTY_CHECK(tx_node.open_device(pty.slave_name, 921600U));

    pb2::transport::UartTransport rx_node;
    PTY_CHECK(rx_node.attach_fd(pty.master, false));

    pb2_preview_block_t blk{};
    blk.first_sample_index = 1000ULL;
    blk.first_timestamp_us = 2000000ULL;
    blk.source_sample_rate_hz = 500U;
    blk.decimation_factor = 4U;
    blk.frame_count = 16U;
    blk.clock_domain = PB2_CLOCK_ESP32_US;
    blk.sample_format = PB2_SAMPLE_FMT_FLOAT32_UV;
    blk.channel_count = 4U;
    for (int i = 0; i < 4; ++i) blk.channel_map[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t payload[PB2_PROTO_MAX_PAYLOAD]{};
    std::uint16_t plen = 0;
    PTY_CHECK(pb2_encode_preview_block(&blk, payload, sizeof(payload), &plen) == PB2_PROTO_OK);

    /* Saturate bounded queue (capacity = 4) */
    pb2_frame_header_t hdr{PB2_PROTO_VERSION, PB2_MSG_PREVIEW_BLOCK, 0U, 1U, 1U, 1U, plen};
    for (int i = 0; i < 6; ++i) {
        hdr.sequence = static_cast<std::uint32_t>(i + 1);
        (void)tx_node.enqueue_frame(hdr, payload);
    }
    PTY_CHECK(tx_node.stats().tx_queue_drops == 2U);

    /* Short throughput burst over PTY (64 preview blocks = 1024 frames) */
    std::size_t received = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < 64; ++b) {
        hdr.sequence = static_cast<std::uint32_t>(100 + b);
        while (!tx_node.enqueue_frame(hdr, payload)) {
            tx_node.poll_once(2, [](const pb2_frame_header_t&, const uint8_t*) {});
            rx_node.poll_once(2, [&](const pb2_frame_header_t&, const uint8_t*) { ++received; });
        }
        tx_node.poll_once(0, [](const pb2_frame_header_t&, const uint8_t*) {});
        rx_node.poll_once(0, [&](const pb2_frame_header_t&, const uint8_t*) { ++received; });
    }
    for (int drain = 0; drain < 30 && received < 68; ++drain) {
        tx_node.poll_once(5, [](const pb2_frame_header_t&, const uint8_t*) {});
        rx_node.poll_once(5, [&](const pb2_frame_header_t&, const uint8_t*) { ++received; });
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    PTY_CHECK(received == 68U);
    std::printf("PTY throughput: 68 blocks (%zu bytes rx) in %lld us\n",
                static_cast<std::size_t>(rx_node.stats().bytes_rx),
                static_cast<long long>(elapsed_us));

    /* Disconnect detection */
    rx_node.close_device();
    ::close(pty.master);
    pty.master = -1;
    const bool alive = tx_node.poll_once(5, [](const pb2_frame_header_t&, const uint8_t*) {});
    PTY_CHECK(!alive);
}

} // namespace

int main() {
    test_pty_fragmented_multiframe_and_resync();
    test_pty_backpressure_disconnect_and_throughput();
    std::printf("pb2_pty_tests: PASS\n");
    return 0;
}
