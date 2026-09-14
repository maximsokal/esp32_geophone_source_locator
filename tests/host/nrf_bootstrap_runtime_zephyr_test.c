#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"
#include "nrf_upstream_target_zephyr.h"

static const struct i2c_target_callbacks *s_callbacks;
static struct k_work *s_submitted_work;
static unsigned s_submit_calls;
static unsigned s_readdress_calls;
static unsigned s_reboot_calls;
static bool s_registered;
static uint16_t s_address;
static int s_readdress_result;
static bool s_readdress_rollback_discovery;

int k_work_submit(struct k_work *work)
{
    s_submitted_work = work;
    ++s_submit_calls;
    return 1;
}

void sys_reboot(int type)
{
    assert(type == 0);
    ++s_reboot_calls;
}

int nrf_upstream_target_zephyr_read_device_id(uint8_t *buffer, size_t capacity, size_t *out_length)
{
    static const uint8_t id[] = {0x10U, 0x20U, 0x30U, 0x40U};
    assert(capacity >= sizeof(id));
    memcpy(buffer, id, sizeof(id));
    *out_length = sizeof(id);
    return 0;
}

int nrf_upstream_target_zephyr_init(const struct i2c_target_callbacks *callbacks)
{
    s_callbacks = callbacks;
    return 0;
}

int nrf_upstream_target_zephyr_register(uint16_t address)
{
    s_registered = true;
    s_address = address;
    return 0;
}

int nrf_upstream_target_zephyr_unregister(void)
{
    s_registered = false;
    s_address = 0U;
    return 0;
}

int nrf_upstream_target_zephyr_readdress(uint16_t address)
{
    ++s_readdress_calls;
    if (s_readdress_result == 0) {
        s_registered = true;
        s_address = address;
        return 0;
    }
    if (s_readdress_rollback_discovery) {
        s_registered = true;
        s_address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
    }
    else {
        s_registered = false;
        s_address = 0U;
    }
    return s_readdress_result;
}

bool nrf_upstream_target_zephyr_is_registered(void)
{
    return s_registered;
}

uint16_t nrf_upstream_target_zephyr_address(void)
{
    return s_registered ? s_address : 0U;
}

static nrf_upstream_assign_t make_assign(uint32_t request_id)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = request_id;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 0x10U;
    assign.hardware_id[1] = 0x20U;
    assign.hardware_id[2] = 0x30U;
    assign.hardware_id[3] = 0x40U;
    assign.role = NRF_UPSTREAM_ROLE_REMOTE;
    assign.node_id = 2U;
    assign.mux_channel = 1U;
    return assign;
}

static nrf_upstream_status_t read_status(void)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;
    nrf_upstream_status_t status;
    assert(s_callbacks != NULL);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    assert(length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
    assert(nrf_upstream_protocol_decode_status(frame, length, &status) == NRF_UPSTREAM_RESULT_OK);
    return status;
}

static void send_assign(const nrf_upstream_assign_t *assign)
{
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
    assert(nrf_upstream_protocol_encode_assign(assign, frame) == NRF_UPSTREAM_RESULT_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void run_submitted_work(void)
{
    struct k_work *work = s_submitted_work;
    assert(work != NULL && work->handler != NULL);
    s_submitted_work = NULL;
    work->handler(work);
}

int main(void)
{
    nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_upstream_assign_t assign;
    unsigned i;

    nrf_node_state_init(&node_state);
    assert(nrf_node_state_mark_ready(&node_state) == NRF_NODE_STATE_OK);
    s_readdress_result = 0;
    s_readdress_rollback_discovery = true;
    assert(nrf_bootstrap_runtime_zephyr_start(&node_state) == 0);
    assert(s_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    status = read_status();
    assert(status.request_id == 0U && status.role == NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    assert(status.flags == NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED);

    assign = make_assign(10U);
    s_readdress_result = -5;
    send_assign(&assign);
    assert(s_readdress_calls == 0U); /* callback must not re-address */
    assert(s_submit_calls == 1U);
    status = read_status();
    assert(status.request_id == 0U); /* worker owns PENDING/final publication */
    run_submitted_work();
    assert(s_readdress_calls == 1U);
    status = read_status();
    assert(status.request_id == 10U && status.result == NRF_UPSTREAM_RESULT_READDRESS_FAILED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    s_readdress_result = 0;
    send_assign(&assign);
    assert(s_readdress_calls == 1U);
    run_submitted_work();
    assert(s_readdress_calls == 2U);
    status = read_status();
    assert(status.request_id == 10U && status.result == NRF_UPSTREAM_RESULT_OK);
    assert(status.role == NRF_UPSTREAM_ROLE_REMOTE && status.node_id == 2U && status.mux_channel == 1U);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_REMOTE);

    /* Queue four idempotent confirmations without running work; the fifth is BUSY. */
    for (i = 0U; i < 4U; ++i) {
        assign.request_id = 20U + i;
        send_assign(&assign);
    }
    assign.request_id = 24U;
    send_assign(&assign);
    status = read_status();
    assert(status.request_id == 24U && status.result == NRF_UPSTREAM_RESULT_BUSY);
    run_submitted_work();
    status = read_status();
    assert(status.request_id == 23U && status.result == NRF_UPSTREAM_RESULT_OK);
    assert(s_readdress_calls == 2U);

    /* Malformed CRC is rejected in callback without scheduling transport work. */
    {
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
        unsigned before = s_submit_calls;
        assign = make_assign(30U);
        assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
        frame[20] ^= 1U;
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        assert(s_submit_calls == before);
        status = read_status();
        assert(status.request_id == 30U && status.result == NRF_UPSTREAM_RESULT_BAD_CRC);
    }

    assert(s_reboot_calls == 0U);
    return 0;
}
