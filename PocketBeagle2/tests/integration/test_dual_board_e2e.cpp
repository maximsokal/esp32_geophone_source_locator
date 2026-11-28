extern "C" {
#include "driver/uart.h"
#include "pb2_protocol.h"
#include "pb2_uart_bridge.h"
#include "seismic_buffer.h"
}

#include "pb2/runtime/pipeline.hpp"
#include "pb2/transport/uart_transport.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

int g_esp_pty_fd = -1;
size_t g_esp_tx_free_bytes = 2048U;

#define E2E_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "Dual-board E2E check failed: %s (%s:%d)\n", \
                         #cond, __FILE__, __LINE__);                        \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace {

struct PtyPair {
    int master = -1;
    char slave_name[128]{};

    PtyPair() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        E2E_CHECK(master >= 0);
        E2E_CHECK(::grantpt(master) == 0);
        E2E_CHECK(::unlockpt(master) == 0);
        const char* name = ::ptsname(master);
        E2E_CHECK(name != nullptr);
        std::snprintf(slave_name, sizeof(slave_name), "%s", name);
    }
    ~PtyPair() {
        if (master >= 0) ::close(master);
    }
};

void send_raw_esp_frame(int fd, std::uint8_t msg_type, std::uint32_t seq,
                        std::uint32_t boot_epoch, std::uint32_t session_epoch,
                        const std::uint8_t* payload, std::uint16_t payload_len) {
    pb2_frame_header_t hdr{};
    hdr.version = PB2_PROTO_VERSION;
    hdr.msg_type = msg_type;
    hdr.flags = 0U;
    hdr.sequence = seq;
    hdr.boot_epoch = boot_epoch;
    hdr.session_epoch = session_epoch;
    hdr.payload_len = payload_len;

    std::uint8_t wire[PB2_PROTO_MAX_COBS_BYTES]{};
    size_t wire_len = 0U;
    E2E_CHECK(pb2_wire_encode(&hdr, payload, wire, sizeof(wire), &wire_len) == PB2_PROTO_OK);
    E2E_CHECK(::write(fd, wire, wire_len) == static_cast<ssize_t>(wire_len));
}

} // namespace

int main() {
    PtyPair pty1;
    g_esp_pty_fd = pty1.master;
    g_esp_tx_free_bytes = 2048U;

    pb2::transport::UartTransport pb2_uart(16U);
    E2E_CHECK(pb2_uart.open_device(pty1.slave_name, 115200U));

    E2E_CHECK(seismic_buffer_init() == ESP_OK);
    E2E_CHECK(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 100U) == ESP_OK);
    E2E_CHECK(pb2_uart_bridge_init() == ESP_OK);

    // 1. HELLO -> PB2 replies CAPABILITIES + ACK
    pb2_hello_payload_t hello{1U, 1U, PB2_PROTO_MAX_PAYLOAD, 0x0FU};
    std::uint8_t hbuf[32]{};
    std::uint16_t hlen = 0U;
    E2E_CHECK(pb2_encode_hello(&hello, hbuf, sizeof(hbuf), &hlen) == PB2_PROTO_OK);
    send_raw_esp_frame(pty1.master, PB2_MSG_HELLO, 1U, 1U, 1U, hbuf, hlen);

    bool saw_hello = false;
    for (int i = 0; i < 10 && !saw_hello; ++i) {
        E2E_CHECK(pb2_uart.poll_once(20, [&](const pb2_frame_header_t& h, const uint8_t*) {
            if (h.msg_type == PB2_MSG_HELLO) {
                saw_hello = true;
                pb2_capabilities_payload_t cap{0xFFU, 921600U, 2U, 4U};
                std::uint8_t cbuf[32]{};
                std::uint16_t clen = 0U;
                E2E_CHECK(pb2_encode_capabilities(&cap, cbuf, sizeof(cbuf), &clen) == PB2_PROTO_OK);
                pb2_frame_header_t chdr{PB2_PROTO_VERSION, PB2_MSG_CAPABILITIES, 0U, 10U, 1U, 1U, clen};
                E2E_CHECK(pb2_uart.enqueue_frame(chdr, cbuf));

                pb2_ack_payload_t ack{};
                ack.acked_sequence = h.sequence;
                ack.acked_msg_type = h.msg_type;
                ack.status_code = 0U;
                std::uint8_t abuf[16]{};
                std::uint16_t alen = 0U;
                E2E_CHECK(pb2_encode_ack(&ack, abuf, sizeof(abuf), &alen) == PB2_PROTO_OK);
                pb2_frame_header_t ahdr{PB2_PROTO_VERSION, PB2_MSG_ACK, 0U, 11U, 1U, 1U, alen};
                E2E_CHECK(pb2_uart.enqueue_frame(ahdr, abuf));
            }
        }));
    }
    E2E_CHECK(saw_hello);
    E2E_CHECK(pb2_uart.poll_once(10, [](const pb2_frame_header_t&, const uint8_t*) {}));
    E2E_CHECK(pb2_uart.stats().frames_tx >= 2U);

    // 2. HEARTBEAT & Duplicate sequence check
    pb2_heartbeat_payload_t hb{1000ULL, 10U, 0U};
    std::uint8_t hbbuf[32]{};
    std::uint16_t hblen = 0U;
    E2E_CHECK(pb2_encode_heartbeat(&hb, hbbuf, sizeof(hbbuf), &hblen) == PB2_PROTO_OK);
    send_raw_esp_frame(pty1.master, PB2_MSG_HEARTBEAT, 2U, 1U, 1U, hbbuf, hblen);
    send_raw_esp_frame(pty1.master, PB2_MSG_HEARTBEAT, 2U, 1U, 1U, hbbuf, hblen);
    E2E_CHECK(pb2_uart.poll_once(20, [](const pb2_frame_header_t&, const uint8_t*) {}));
    E2E_CHECK(pb2_uart.stats().duplicate_seq_count >= 1U);

    // 3. Production ESP32 seismic_buffer -> pb2_uart_bridge_poll_once() -> PTY -> UartTransport
    for (int i = 0; i < 16; ++i) {
        const float ch[4] = {
            static_cast<float>(100 + i),
            static_cast<float>(-100 - i),
            static_cast<float>(200 + i),
            static_cast<float>(-200 - i)
        };
        E2E_CHECK(seismic_buffer_push_indexed(static_cast<int64_t>(i * 10000LL), static_cast<uint64_t>(i), ch) == ESP_OK);
    }
    E2E_CHECK(pb2_uart_bridge_poll_once() == ESP_OK);

    pb2_preview_block_t rx_blk{};
    bool saw_preview = false;
    for (int i = 0; i < 10 && !saw_preview; ++i) {
        E2E_CHECK(pb2_uart.poll_once(20, [&](const pb2_frame_header_t& h, const uint8_t* p) {
            if (h.msg_type == PB2_MSG_PREVIEW_BLOCK) {
                E2E_CHECK(pb2_decode_preview_block(p, h.payload_len, &rx_blk) == PB2_PROTO_OK);
                saw_preview = true;
            }
        }));
    }
    E2E_CHECK(saw_preview);
    E2E_CHECK(rx_blk.frame_count == 16U);
    E2E_CHECK(rx_blk.first_sample_index == 0ULL);
    E2E_CHECK(rx_blk.clock_domain == PB2_CLOCK_ESP32_US);
    E2E_CHECK(rx_blk.sample_format == PB2_SAMPLE_FMT_FLOAT32_UV);
    E2E_CHECK(rx_blk.channel_map[0] == 1U && rx_blk.channel_map[3] == 4U);

    // 4. Reboot / session epoch rollover on ESP32 generation change
    E2E_CHECK(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 100U) == ESP_OK);
    E2E_CHECK(pb2_uart_bridge_poll_once() == ESP_OK);
    const float ch2[4] = {42.0f, -42.0f, 84.0f, -84.0f};
    E2E_CHECK(seismic_buffer_push_indexed(999000LL, 0ULL, ch2) == ESP_OK);
    E2E_CHECK(pb2_uart_bridge_poll_once() == ESP_OK);
    for (int i = 0; i < 10; ++i) {
        E2E_CHECK(pb2_uart.poll_once(10, [](const pb2_frame_header_t&, const uint8_t*) {}));
    }
    E2E_CHECK(pb2_uart.stats().session_rollovers >= 1U);

    // 5. Non-blocking acquisition when UART TX buffer is full (tx_drops increments, push still OK)
    g_esp_tx_free_bytes = 8U;
    E2E_CHECK(seismic_buffer_push_indexed(1009000LL, 1ULL, ch2) == ESP_OK);
    E2E_CHECK(pb2_uart_bridge_poll_once() == ESP_OK);
    pb2_uart_bridge_stats_t bstats{};
    pb2_uart_bridge_get_stats(&bstats);
    E2E_CHECK(bstats.tx_drops >= 1ULL);
    g_esp_tx_free_bytes = 2048U;

    // 6. CRC error injection
    std::uint8_t bad_wire[16] = {0x05, 'P', 'B', '2', 0xFF, 0x00};
    E2E_CHECK(::write(pty1.master, bad_wire, 6) == 6);
    E2E_CHECK(pb2_uart.poll_once(20, [](const pb2_frame_header_t&, const uint8_t*) {}));
    E2E_CHECK((pb2_uart.stats().cobs_errors + pb2_uart.stats().crc_errors + pb2_uart.stats().header_errors) >= 1U);

    // 7. Disconnect + reconnect recovery
    ::close(pty1.master);
    pty1.master = -1;
    bool disconnected = !pb2_uart.poll_once(20, [](const pb2_frame_header_t&, const uint8_t*) {});
    E2E_CHECK(disconnected);
    pb2_uart.close_device();

    PtyPair pty2;
    g_esp_pty_fd = pty2.master;
    E2E_CHECK(pb2_uart.open_device(pty2.slave_name, 115200U));
    send_raw_esp_frame(pty2.master, PB2_MSG_HEARTBEAT, 99U, 2U, 1U, hbbuf, hblen);
    bool reconnected_rx = false;
    for (int i = 0; i < 10 && !reconnected_rx; ++i) {
        E2E_CHECK(pb2_uart.poll_once(20, [&](const pb2_frame_header_t& h, const uint8_t*) {
            if (h.msg_type == PB2_MSG_HEARTBEAT && h.sequence == 99U) {
                reconnected_rx = true;
            }
        }));
    }
    E2E_CHECK(reconnected_rx);

    // 8. Production RuntimePipeline::handle_uart_rx_frame bidirectional test over PTY
    {
        PtyPair pty3;
        pb2::transport::UartTransport pb2_side(16U);
        E2E_CHECK(pb2_side.open_device(pty3.slave_name, 115200U));
        pb2::runtime::PipelineConfig pcfg{};
        pcfg.profile = pb2::acquisition::AdcWiringProfile::SerialDualDout;
        pcfg.uart_baud = 115200U;
        pb2::runtime::RuntimePipeline pipe(pcfg);
        std::atomic<bool> remote_stop{false};

        send_raw_esp_frame(pty3.master, PB2_MSG_PING, 1U, 1U, 1U, nullptr, 0U);
        bool saw_ping = false;
        for (int i = 0; i < 10 && !saw_ping; ++i) {
            E2E_CHECK(pb2_side.poll_once(20, [&](const pb2_frame_header_t& h, const uint8_t* p) {
                saw_ping = true;
                pipe.handle_uart_rx_frame(pb2_side, h, p, remote_stop);
            }));
        }
        E2E_CHECK(saw_ping);
        E2E_CHECK(pb2_side.poll_once(10, [](const pb2_frame_header_t&, const uint8_t*) {}));
        E2E_CHECK(pb2_side.stats().frames_tx >= 1U);
    }

    std::printf("pb2_dual_board_e2e_tests: ALL PASSED\n");
    return 0;
}
