#include "nrf_bootstrap_runtime_zephyr.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "nrf_bootstrap.h"
#include "nrf_boot_diagnostics.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_cs_control_runtime.h"
#include "nrf_cs_cycle_runtime.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_hci_workqueue.h"
#include "nrf_read_window.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"
#include "nrf_upstream_target_zephyr.h"

#define NRF_BOOTSTRAP_QUEUE_DEPTH 4U
#define NRF_CS_EXECUTE_QUEUE_DEPTH 2U

typedef struct {
    uint32_t sequence;
    nrf_upstream_assign_t assign;
} assign_work_item_t;

typedef struct {
    uint32_t sequence;
    nrf_cs_control_request_t request;
} cs_execute_work_item_t;

K_MSGQ_DEFINE(s_assign_queue, sizeof(assign_work_item_t), NRF_BOOTSTRAP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(s_cs_execute_queue, sizeof(cs_execute_work_item_t), NRF_CS_EXECUTE_QUEUE_DEPTH, 4);

static struct k_work s_assign_work;
static struct k_work s_cs_execute_work;
static struct k_spinlock s_assign_queue_lock;
static struct k_spinlock s_cs_execute_queue_lock;
static struct k_spinlock s_status_lock;
static struct k_spinlock s_cs_lock;
static nrf_bootstrap_t s_bootstrap;
static nrf_upstream_status_t s_published_status;
static nrf_cs_control_runtime_t s_cs_control;
static uint8_t s_read_frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
static uint8_t s_log_response_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
static uint32_t s_log_response_length;
static bool s_log_response_selected;
/* Copy of the last response handed to the master, served again window by window.
 * Touched only from the TWIS callbacks, which never run concurrently. */
static nrf_read_window_target_t s_read_window;
static bool s_started;
static uint32_t s_assign_sequence;
static uint32_t s_cs_execute_sequence;
/* Advisory callback snapshots: volatile saturating counters avoid taking a
 * lock in the target callback; readers may observe neighboring fields mid-update. */
static volatile uint32_t s_twis_write_received_count;
static volatile uint32_t s_twis_write_received_bytes;
static volatile uint32_t s_twis_read_requested_count;
static volatile uint32_t s_twis_transaction_count;
static volatile uint32_t s_twis_callback_error_count;
static volatile bool s_twis_first_transaction_seen;
static volatile uint8_t s_twis_first_transaction_kind;
static volatile bool s_twis_last_request_type_valid;
static volatile uint8_t s_twis_last_request_type;
static volatile uint32_t s_twis_last_request_type_count;
static volatile uint32_t s_role_assignment_sequence;
static volatile uint32_t s_role_assignment_node_id;
static volatile uint8_t s_role_assignment_role;
static nrf_boot_twis_record_ring_t s_twis_record_ring;

static void increment_saturating(volatile uint32_t *counter)
{
    const uint32_t current = *counter;
    if (current != UINT32_MAX) {
        *counter = current + 1U;
    }
}

static void add_saturating(volatile uint32_t *counter, uint32_t amount)
{
    const uint32_t current = *counter;
    if (amount > UINT32_MAX - current) {
        *counter = UINT32_MAX;
    } else {
        *counter = current + amount;
    }
}

static uint32_t next_twis_transaction_index(void)
{
    increment_saturating(&s_twis_transaction_count);
    return s_twis_transaction_count;
}

static void record_twis_transaction(
    uint32_t index,
    nrf_boot_twis_direction_t direction,
    const uint8_t *bytes,
    uint32_t length,
    int32_t result)
{
    if (result != 0) {
        increment_saturating(&s_twis_callback_error_count);
    }

    {
        nrf_boot_twis_record_t record = {0};
        uint32_t prefix_length = bytes == NULL ? 0U : length;

        if (prefix_length > NRF_BOOT_TRANSACTION_PREFIX_CAPACITY) {
            prefix_length = NRF_BOOT_TRANSACTION_PREFIX_CAPACITY;
        }
        record.index = index;
        record.length = length;
        record.direction = (uint8_t)direction;
        record.prefix_length = (uint8_t)prefix_length;
        record.result = result;
        for (uint32_t byte_index = 0U; byte_index < prefix_length; ++byte_index) {
            record.prefix[byte_index] = bytes == NULL ? 0U : bytes[byte_index];
        }
        (void)nrf_boot_twis_record_ring_push(&s_twis_record_ring, &record);
    }
}

void nrf_bootstrap_runtime_zephyr_record_twis_hardware(
    const struct nrf_boot_twis_record *record)
{
    if (record != NULL) {
        nrf_boot_twis_record_t snapshot = *record;
        snapshot.index = s_twis_transaction_count;
        (void)nrf_boot_twis_record_ring_push(&s_twis_record_ring, &snapshot);
    }
}

static void record_first_twis_transaction(
    nrf_bootstrap_twis_transaction_kind_t kind)
{
    if (!s_twis_first_transaction_seen) {
        s_twis_first_transaction_kind = (uint8_t)kind;
        s_twis_first_transaction_seen = true;
    }
}

static void record_twis_request_type(const uint8_t *frame, uint32_t length)
{
    uint8_t type;

    if (frame == NULL || length <= 3U) {
        s_twis_last_request_type_valid = false;
        s_twis_last_request_type_count = 0U;
        return;
    }

    type = frame[3];
    if (s_twis_last_request_type_valid && s_twis_last_request_type == type) {
        increment_saturating(&s_twis_last_request_type_count);
    } else {
        s_twis_last_request_type = type;
        s_twis_last_request_type_count = 1U;
        s_twis_last_request_type_valid = true;
    }
}

uint32_t nrf_bootstrap_runtime_zephyr_response_length(void)
{
    return s_read_window.length;
}

void nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(
    nrf_bootstrap_twis_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics->transaction_count = s_twis_transaction_count;
    diagnostics->write_received_count = s_twis_write_received_count;
    diagnostics->write_received_bytes = s_twis_write_received_bytes;
    diagnostics->read_requested_count = s_twis_read_requested_count;
    diagnostics->callback_error_count = s_twis_callback_error_count;
    diagnostics->first_transaction_seen = s_twis_first_transaction_seen;
    diagnostics->first_transaction_kind =
        (nrf_bootstrap_twis_transaction_kind_t)s_twis_first_transaction_kind;
    diagnostics->last_request_type_valid = s_twis_last_request_type_valid;
    diagnostics->last_request_type = s_twis_last_request_type;
    diagnostics->last_request_type_count = s_twis_last_request_type_count;
    diagnostics->role_assignment_sequence = s_role_assignment_sequence;
    diagnostics->role_assignment_node_id = s_role_assignment_node_id;
    diagnostics->role_assignment_role = s_role_assignment_role;
    diagnostics->diagnostic_ring_dropped_total = s_twis_record_ring.dropped_total;
}

bool nrf_bootstrap_runtime_zephyr_drain_twis_record(
    struct nrf_boot_twis_record *record)
{
    return nrf_boot_twis_record_ring_pop(
        &s_twis_record_ring,
        record);
}

static int runtime_readdress(void *context, uint8_t address){(void)context;return nrf_upstream_target_zephyr_readdress(address);}
static bool runtime_target_is_registered(void *context){(void)context;return nrf_upstream_target_zephyr_is_registered();}
static uint8_t runtime_target_address(void *context){(void)context;return (uint8_t)nrf_upstream_target_zephyr_address();}
static void runtime_cold_reboot(void *context){(void)context;sys_reboot(SYS_REBOOT_COLD);}

static void publish_status(const nrf_upstream_status_t *status){k_spinlock_key_t key=k_spin_lock(&s_status_lock);s_published_status=*status;k_spin_unlock(&s_status_lock,key);}
static void publish_core_status(void){nrf_upstream_status_t status;if(nrf_bootstrap_make_status(&s_bootstrap,&status)==NRF_UPSTREAM_RESULT_OK)publish_status(&status);}
static void publish_core_status_with_result(nrf_upstream_result_t result){nrf_upstream_status_t status;if(nrf_bootstrap_make_status(&s_bootstrap,&status)==NRF_UPSTREAM_RESULT_OK){status.result=(uint8_t)result;publish_status(&status);}}
static void publish_result_context(uint32_t request_id,nrf_upstream_result_t result){k_spinlock_key_t key=k_spin_lock(&s_status_lock);s_published_status.request_id=request_id;s_published_status.result=(uint8_t)result;k_spin_unlock(&s_status_lock,key);}
static void assignment_runtime_failure(void){publish_core_status_with_result(NRF_UPSTREAM_RESULT_INTERNAL_ERROR);runtime_cold_reboot(NULL);}

static uint32_t next_sequence(uint32_t *sequence)
{
    ++*sequence;
    if (*sequence == 0U) {
        ++*sequence;
    }
    return *sequence;
}

static bool remove_assign_work_item(uint32_t sequence)
{
    assign_work_item_t kept[NRF_BOOTSTRAP_QUEUE_DEPTH];
    assign_work_item_t item;
    size_t kept_count = 0U;
    bool removed = false;

    while (k_msgq_get(&s_assign_queue, &item, K_NO_WAIT) == 0) {
        if (!removed && item.sequence == sequence) {
            removed = true;
        } else {
            kept[kept_count++] = item;
        }
    }
    for (size_t index = 0U; index < kept_count; ++index) {
        (void)k_msgq_put(&s_assign_queue, &kept[index], K_NO_WAIT);
    }
    return removed;
}

static bool remove_cs_execute_work_item(uint32_t sequence)
{
    cs_execute_work_item_t kept[NRF_CS_EXECUTE_QUEUE_DEPTH];
    cs_execute_work_item_t item;
    size_t kept_count = 0U;
    bool removed = false;

    while (k_msgq_get(&s_cs_execute_queue, &item, K_NO_WAIT) == 0) {
        if (!removed && item.sequence == sequence) {
            removed = true;
        } else {
            kept[kept_count++] = item;
        }
    }
    for (size_t index = 0U; index < kept_count; ++index) {
        (void)k_msgq_put(&s_cs_execute_queue, &kept[index], K_NO_WAIT);
    }
    return removed;
}

static void cs_runtime_complete(const nrf_cs_step_t *step,const nrf_cs_measurement_t *measurement,void *context)
{
    k_spinlock_key_t key;
    (void)context;
    key=k_spin_lock(&s_cs_lock);
    (void)nrf_cs_control_runtime_complete(&s_cs_control,step,measurement);
    k_spin_unlock(&s_cs_lock,key);
    if (step != NULL && measurement != NULL && measurement->outcome != NRF_CS_OUTCOME_SUCCESS) {
        if (measurement->outcome == NRF_CS_OUTCOME_TIMEOUT) {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_CS_TIMEOUT,
                step->attempt_id,
                step->timeout_ms);
        } else {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_CS_EXECUTE_FAILED,
                step->attempt_id,
                (uint32_t)measurement->outcome);
        }
    }
}

static void cs_execute_work_handler(struct k_work *work)
{
    cs_execute_work_item_t item;
    (void)work;
    for (;;) {
        k_spinlock_key_t queue_key = k_spin_lock(&s_cs_execute_queue_lock);
        const int got = k_msgq_get(&s_cs_execute_queue, &item, K_NO_WAIT);
        k_spin_unlock(&s_cs_execute_queue_lock, queue_key);
        if (got != 0) {
            break;
        }
        nrf_cs_control_request_t request = item.request;
        nrf_cs_control_result_t status;
        nrf_cs_measurement_t failure={.outcome=NRF_CS_OUTCOME_TRANSPORT_ERROR};
        bool start_required=false;
        k_spinlock_key_t key=k_spin_lock(&s_cs_lock);
        int state_result=nrf_cs_control_runtime_accept_execute(&s_cs_control,&request,&status,&start_required);
        k_spin_unlock(&s_cs_lock,key);
        if(state_result!=0||!start_required)continue;
        int execute_result=nrf_cs_pair_runtime_zephyr_execute(&request.step,cs_runtime_complete,NULL);
        if(execute_result!=0){
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_CS_EXECUTE_FAILED,
                request.step.attempt_id,
                (uint32_t)execute_result);
            key=k_spin_lock(&s_cs_lock);
            (void)nrf_cs_control_runtime_complete(&s_cs_control,&request.step,&failure);
            k_spin_unlock(&s_cs_lock,key);
        }
    }
}

static void assign_work_handler(struct k_work *work)
{
    assign_work_item_t item;
    nrf_upstream_status_t previous_status;
    nrf_upstream_status_t current_status;
    (void)work;
    for (;;) {
        k_spinlock_key_t queue_key = k_spin_lock(&s_assign_queue_lock);
        const int got = k_msgq_get(&s_assign_queue, &item, K_NO_WAIT);
        k_spin_unlock(&s_assign_queue_lock, queue_key);
        if (got != 0) {
            break;
        }
        nrf_upstream_assign_t assign = item.assign;
        nrf_upstream_result_t result;
        k_spinlock_key_t status_key=k_spin_lock(&s_status_lock);
        previous_status=s_published_status;
        k_spin_unlock(&s_status_lock,status_key);
        publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_PENDING);
        result=nrf_bootstrap_process_assign(&s_bootstrap,&assign);
        if(result==NRF_UPSTREAM_RESULT_OK&&assign.role==NRF_UPSTREAM_ROLE_REMOTE){if(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign)!=0){assignment_runtime_failure();continue;}}
        if (result == NRF_UPSTREAM_RESULT_OK) {
            const int cs_pair_result = nrf_cs_pair_runtime_zephyr_set_node_id(assign.node_id);
            /* An unstarted optional CS subsystem returns -EAGAIN; main.c
             * already logged BOOT_FAILED. Keep sensors and TWIS assigned,
             * but still reboot on conflicting identity/invariant failures. */
            if (cs_pair_result != 0 && cs_pair_result != -EAGAIN) {
                assignment_runtime_failure();
                continue;
            }
        }
        if(result==NRF_UPSTREAM_RESULT_OK){k_spinlock_key_t key=k_spin_lock(&s_cs_lock);int cs_result=nrf_cs_control_runtime_assign_node(&s_cs_control,assign.node_id);k_spin_unlock(&s_cs_lock,key);if(cs_result!=0){assignment_runtime_failure();continue;}}
        if(result==NRF_UPSTREAM_RESULT_OK&&nrf_bootstrap_make_status(&s_bootstrap,&current_status)==NRF_UPSTREAM_RESULT_OK&&previous_status.role!=current_status.role){
            if(previous_status.role!=NRF_UPSTREAM_ROLE_UNASSIGNED){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_ROLE_LOST,previous_status.node_id,previous_status.role);}
            if(current_status.role!=NRF_UPSTREAM_ROLE_UNASSIGNED){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_ROLE_ASSIGNED,current_status.node_id,current_status.role);}
        }
        if (result == NRF_UPSTREAM_RESULT_OK) {
            s_role_assignment_node_id = assign.node_id;
            s_role_assignment_role = (uint8_t)assign.role;
            increment_saturating(&s_role_assignment_sequence);
        }
        publish_core_status();
    }
}

static void select_log_error_response(uint8_t *frame,size_t frame_length,nrf_log_error_t error)
{
    nrf_upstream_status_t status;
    uint32_t request_id=nrf_upstream_protocol_request_id_hint(frame,frame_length);
    k_spinlock_key_t key=k_spin_lock(&s_status_lock);
    status=s_published_status;
    k_spin_unlock(&s_status_lock,key);
    status.request_id=request_id;
    status.result=NRF_UPSTREAM_RESULT_INVALID_FRAME;
    if(nrf_upstream_protocol_encode_status(&status,s_log_response_frame)==NRF_UPSTREAM_RESULT_OK){
        s_log_response_length=NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH;
        s_log_response_selected=true;
    }
    nrf_log_runtime_zephyr_append_event(
        NRF_LOG_EVENT_TWIS_INVALID_FRAME,
        frame!=NULL&&frame_length>3U?frame[3]:0U,
        (uint32_t)error);
}

static bool handle_log_write(
    uint8_t *frame,
    size_t frame_length,
    int32_t *transaction_result)
{
    nrf_log_error_t error;
    size_t response_length=0U;

    if(frame==NULL||frame_length<=3U||
       (frame[3]!=NRF_LOG_MESSAGE_STATUS_REQUEST&&frame[3]!=NRF_LOG_MESSAGE_READ_REQUEST&&
        frame[3]!=NRF_LOG_MESSAGE_FIRMWARE_REQUEST)){
        return false;
    }
    if(frame[3]==NRF_LOG_MESSAGE_STATUS_REQUEST){
        nrf_log_status_request_t request;
        error=nrf_log_protocol_decode_status_request(frame,frame_length,&request);
        if(error==NRF_LOG_ERROR_OK){
            error=nrf_log_runtime_zephyr_build_status(
                request.request_id,
                s_log_response_frame,
                sizeof(s_log_response_frame),
                &response_length);
        }
    }else if(frame[3]==NRF_LOG_MESSAGE_FIRMWARE_REQUEST){
        nrf_log_status_request_t request;
        error=nrf_log_protocol_decode_firmware_request(frame,frame_length,&request);
        if(error==NRF_LOG_ERROR_OK){
            error=nrf_log_runtime_zephyr_build_firmware(
                request.request_id, s_log_response_frame,
                sizeof(s_log_response_frame), &response_length);
        }
    }else{
        nrf_log_read_request_t request;
        error=nrf_log_protocol_decode_read_request(frame,frame_length,&request);
        if(error==NRF_LOG_ERROR_OK){
            error=nrf_log_runtime_zephyr_build_block(
                &request,
                s_log_response_frame,
                sizeof(s_log_response_frame),
                &response_length);
        }
    }
    if(error!=NRF_LOG_ERROR_OK){
        select_log_error_response(frame,frame_length,error);
        *transaction_result = -EBADMSG;
        return true;
    }
    s_log_response_length=(uint32_t)response_length;
    s_log_response_selected=true;
    return true;
}

/*
 * Autonomous Channel Sounding cycle (see nrf_cs_cycle_runtime.h). The runtime is shared between the
 * TWIS callbacks (plan intake, replies) and the main loop (tick, BLE attempts), so every call is made
 * under s_cycle_lock; the BLE attempt itself is started outside the lock.
 */
static nrf_cs_cycle_runtime_t s_cycle;
static struct k_spinlock s_cycle_lock;
static uint8_t s_cycle_reply[NRF_CS_CYCLE_RESULT_LENGTH];
static uint32_t s_cycle_reply_length;
static bool s_cycle_reply_selected;
static uint8_t s_cycle_status_frame[NRF_CS_CYCLE_STATUS_LENGTH];
static bool s_cycle_radio_on;

static uint64_t cycle_now_ms(void *context)
{
    (void)context;
    return (uint64_t)k_uptime_get();
}

static void cycle_abort_attempt(void *context)
{
    (void)context;
    /* The step's own timeout (pair_timeout_ms) ends an attempt before its window closes. */
}

static void cycle_step_done(const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement, void *context)
{
    (void)context;
    k_spinlock_key_t key = k_spin_lock(&s_cycle_lock);
    (void)nrf_cs_cycle_runtime_step_done(&s_cycle, step, measurement);
    k_spin_unlock(&s_cycle_lock, key);
}

void nrf_bootstrap_runtime_zephyr_cs_cycle_tick(void)
{
    nrf_cs_step_t step;
    k_spinlock_key_t key = k_spin_lock(&s_cycle_lock);
    nrf_cs_cycle_runtime_tick(&s_cycle);
    const bool running = nrf_cs_cycle_runtime_running(&s_cycle);
    const bool start = nrf_cs_cycle_runtime_take_start(&s_cycle, &step);
    k_spin_unlock(&s_cycle_lock, key);
    /* Radio only while the cycle runs: reflectors advertise from GO on, nothing before or after. */
    if (running != s_cycle_radio_on && nrf_cs_pair_runtime_zephyr_set_radio_enabled(running) == 0) {
        s_cycle_radio_on = running;
    }
    if (!start) {
        return;
    }
    int result = nrf_cs_pair_runtime_zephyr_execute(&step, cycle_step_done, NULL);
    if (result == -EBUSY) {
        result = 1; /* a connection is still closing: retry shortly, it is not a failed attempt */
    } else if (result != 0) {
        nrf_log_runtime_zephyr_append_event(
            NRF_LOG_EVENT_CS_EXECUTE_FAILED, step.attempt_id, (uint32_t)result);
    }
    key = k_spin_lock(&s_cycle_lock);
    nrf_cs_cycle_runtime_start_result(&s_cycle, &step, result);
    k_spin_unlock(&s_cycle_lock, key);
}

static bool cycle_local_node(nrf_cs_node_id_t *node)
{
    nrf_upstream_status_t status;
    k_spinlock_key_t key = k_spin_lock(&s_status_lock);
    status = s_published_status;
    k_spin_unlock(&s_status_lock, key);
    if (status.role == NRF_UPSTREAM_ROLE_UNASSIGNED) {
        return false;
    }
    *node = (nrf_cs_node_id_t)status.node_id;
    return true;
}

/* Handles the cycle frames the ESP32 writes. Returns true when the frame was one of them. */
static bool handle_cycle_write(const uint8_t *frame, size_t length, int32_t *transaction_result)
{
    int rc = -EINVAL;
    nrf_cs_node_id_t local;
    k_spinlock_key_t key;

    if (frame == NULL || length < 4U) {
        return false;
    }
    switch (frame[3]) {
    case NRF_CS_CYCLE_MESSAGE_PLAN: {
        nrf_cs_cycle_plan_t plan;
        if (nrf_cs_cycle_decode_plan(frame, length, &plan) == 0 && cycle_local_node(&local)) {
            key = k_spin_lock(&s_cycle_lock);
            rc = nrf_cs_cycle_runtime_accept_plan(&s_cycle, local, &plan);
            k_spin_unlock(&s_cycle_lock, key);
        }
        break;
    }
    case NRF_CS_CYCLE_MESSAGE_PLAN_STEPS: {
        nrf_cs_cycle_plan_steps_t chunk;
        if (nrf_cs_cycle_decode_plan_steps(frame, length, &chunk) == 0) {
            key = k_spin_lock(&s_cycle_lock);
            rc = nrf_cs_cycle_runtime_accept_steps(&s_cycle, &chunk);
            k_spin_unlock(&s_cycle_lock, key);
        }
        break;
    }
    case NRF_CS_CYCLE_MESSAGE_GO: {
        nrf_cs_cycle_go_t go;
        if (nrf_cs_cycle_decode_go(frame, length, &go) == 0) {
            key = k_spin_lock(&s_cycle_lock);
            rc = nrf_cs_cycle_runtime_go(&s_cycle, &go);
            k_spin_unlock(&s_cycle_lock, key);
        }
        break;
    }
    case NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST: {
        nrf_cs_cycle_result_request_t request;
        if (nrf_cs_cycle_decode_result_request(frame, length, &request) != 0) {
            break;
        }
        if (request.page == NRF_CS_CYCLE_RESULT_RELEASE_PAGE) {
            key = k_spin_lock(&s_cycle_lock);
            nrf_cs_cycle_runtime_release(&s_cycle);
            k_spin_unlock(&s_cycle_lock, key);
            rc = 0;
            break;
        }
        nrf_cs_cycle_result_t page;
        key = k_spin_lock(&s_cycle_lock);
        rc = nrf_cs_cycle_runtime_result_page(&s_cycle, request.request_id, request.page, &page);
        k_spin_unlock(&s_cycle_lock, key);
        if (rc == 0 && (page.cycle_id != request.cycle_id ||
                nrf_cs_cycle_encode_result(&page, s_cycle_reply) != 0)) {
            rc = -EINVAL;
        }
        if (rc == 0) {
            s_cycle_reply_length = NRF_CS_CYCLE_RESULT_LENGTH;
            s_cycle_reply_selected = true;
        }
        break;
    }
    default:
        return false;
    }
    if (rc != 0) {
        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME, frame[3], (uint32_t)(-rc));
        *transaction_result = rc;
    }
    return true;
}

static void target_write_received(struct i2c_target_config *config,uint8_t *ptr,uint32_t len)
{
    nrf_upstream_assign_t assign;
    nrf_upstream_sensor_refresh_t sensor_request;
    nrf_upstream_height_reference_t height_reference_request;
    nrf_upstream_imu_calibrate_t imu_calibration_request;
    nrf_cs_control_request_t cs_request;
    nrf_upstream_result_t result;
    uint32_t transaction_index;
    uint32_t request_id;
    k_spinlock_key_t key;
    int submit_result;
    int32_t transaction_result = 0;
    (void)config;

    transaction_index = next_twis_transaction_index();
    increment_saturating(&s_twis_write_received_count);
    add_saturating(&s_twis_write_received_bytes, len);
    record_first_twis_transaction(NRF_BOOTSTRAP_TWIS_TRANSACTION_WRITE_RECEIVED);
    record_twis_request_type(ptr, len);

    s_log_response_selected=false;
    s_cycle_reply_selected=false;
    /* Only the window request that follows a read may leave a window armed. */
    nrf_read_window_target_disarm(&s_read_window);
    if (len == 0U) {
        goto record_transaction;
    }

    if (ptr != NULL && len > 3U && ptr[3] == NRF_UPSTREAM_MESSAGE_READ_WINDOW) {
        nrf_upstream_read_window_t window;
        result = nrf_upstream_protocol_decode_read_window(ptr, (size_t)len, &window);
        if (result == NRF_UPSTREAM_RESULT_OK) {
            if (!nrf_read_window_target_arm(&s_read_window, window.offset)) {
                /* No response to window, or offset past its end. */
                nrf_log_runtime_zephyr_append_event(
                    NRF_LOG_EVENT_TWIS_INVALID_FRAME, ptr[3], (uint32_t)window.offset);
                transaction_result = -EINVAL;
            }
        } else {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_TWIS_INVALID_FRAME, ptr[3], (uint32_t)result);
            transaction_result = (int32_t)result;
        }
        goto record_transaction;
    }

    if(handle_log_write(ptr,(size_t)len,&transaction_result))goto record_transaction;
    if(handle_cycle_write(ptr,(size_t)len,&transaction_result))goto record_transaction;

    /* Dispatch by type so a short control frame cannot overwrite ASSIGN status. */
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_CS_CONTROL_MESSAGE_EXECUTE){
        cs_execute_work_item_t item;
        k_spinlock_key_t queue_key;
        transaction_result = nrf_cs_control_decode_execute(
            ptr, (size_t)len, &cs_request);
        if(transaction_result!=0){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],1U);goto record_transaction;}
        queue_key = k_spin_lock(&s_cs_execute_queue_lock);
        item.sequence = next_sequence(&s_cs_execute_sequence);
        item.request = cs_request;
        if(k_msgq_put(&s_cs_execute_queue,&item,K_NO_WAIT)!=0){
            k_spin_unlock(&s_cs_execute_queue_lock,queue_key);
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_BUSY,ptr[3],NRF_CS_EXECUTE_QUEUE_DEPTH);
            key=k_spin_lock(&s_cs_lock);
            (void)nrf_cs_control_runtime_record_enqueue_result(
                &s_cs_control,&cs_request,NRF_CS_CONTROL_RESULT_BUSY);
            k_spin_unlock(&s_cs_lock,key);
            transaction_result=-EBUSY;
            goto record_transaction;
        }
        submit_result = k_work_submit(&s_cs_execute_work);
        if(submit_result<0){
            (void)remove_cs_execute_work_item(item.sequence);
        }
        k_spin_unlock(&s_cs_execute_queue_lock,queue_key);
        if(submit_result<0){
            key=k_spin_lock(&s_cs_lock);
            (void)nrf_cs_control_runtime_record_enqueue_result(
                &s_cs_control,&cs_request,NRF_CS_CONTROL_RESULT_INTERNAL_ERROR);
            k_spin_unlock(&s_cs_lock,key);
        }
        transaction_result = submit_result < 0 ? submit_result : 0;
        goto record_transaction;
    }
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST){
        transaction_result = nrf_cs_control_decode_status_request(
            ptr, (size_t)len, &cs_request);
        if(transaction_result==0){key=k_spin_lock(&s_cs_lock);transaction_result=nrf_cs_control_runtime_select_result(&s_cs_control,&cs_request);k_spin_unlock(&s_cs_lock,key);}else{nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],1U);}
        goto record_transaction;
    }
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH){
        result=nrf_upstream_protocol_decode_sensor_refresh(ptr,(size_t)len,&sensor_request);
        if(result==NRF_UPSTREAM_RESULT_OK)transaction_result=nrf_sensor_upstream_runtime_zephyr_request_refresh(&sensor_request);
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr!=NULL&&len>3U?ptr[3]:0U,(uint32_t)result);
        if(result!=NRF_UPSTREAM_RESULT_OK)transaction_result=(int32_t)result;
        goto record_transaction;
    }
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_UPSTREAM_MESSAGE_SENSOR_SCAN){
        result=nrf_upstream_protocol_decode_sensor_scan(ptr,(size_t)len,&sensor_request);
        if(result==NRF_UPSTREAM_RESULT_OK){
            transaction_result=nrf_sensor_upstream_runtime_zephyr_request_scan(&sensor_request);
            if(transaction_result!=0)nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_SENSOR_SCAN_DONE,sensor_request.request_id,(uint32_t)transaction_result);
        }
        else { nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],(uint32_t)result); transaction_result=(int32_t)result; }
        goto record_transaction;
    }
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE){
        result=nrf_upstream_protocol_decode_height_reference(ptr,(size_t)len,&height_reference_request);
        if(result==NRF_UPSTREAM_RESULT_OK)transaction_result=nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height_reference_request);
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],(uint32_t)result);
        if(result!=NRF_UPSTREAM_RESULT_OK)transaction_result=(int32_t)result;
        goto record_transaction;
    }
    if(ptr!=NULL&&len>3U&&ptr[3]==NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE){
        result=nrf_upstream_protocol_decode_imu_calibrate(ptr,(size_t)len,&imu_calibration_request);
        if(result==NRF_UPSTREAM_RESULT_OK)transaction_result=nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(&imu_calibration_request);
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],(uint32_t)result);
        if(result!=NRF_UPSTREAM_RESULT_OK)transaction_result=(int32_t)result;
        goto record_transaction;
    }
    request_id=nrf_upstream_protocol_request_id_hint(ptr,(size_t)len);
    result=nrf_upstream_protocol_decode_assign(ptr,(size_t)len,&assign);
    if(result!=NRF_UPSTREAM_RESULT_OK){
        if(result==NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_UNKNOWN_TYPE,ptr!=NULL&&len>3U?ptr[3]:0U,len);}
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr!=NULL&&len>3U?ptr[3]:0U,(uint32_t)result);
        publish_result_context(request_id,result);
        transaction_result=(int32_t)result;
        goto record_transaction;
    }
    {
        assign_work_item_t item;
        k_spinlock_key_t queue_key = k_spin_lock(&s_assign_queue_lock);
        item.sequence = next_sequence(&s_assign_sequence);
        item.assign = assign;
        if(k_msgq_put(&s_assign_queue,&item,K_NO_WAIT)!=0){
            k_spin_unlock(&s_assign_queue_lock,queue_key);
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_BUSY,ptr[3],NRF_BOOTSTRAP_QUEUE_DEPTH);
            publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_BUSY);
            transaction_result=(int32_t)NRF_UPSTREAM_RESULT_BUSY;
            goto record_transaction;
        }
        submit_result=nrf_hci_workqueue_submit(&s_assign_work);
        if(submit_result<0){
            (void)remove_assign_work_item(item.sequence);
        }
        k_spin_unlock(&s_assign_queue_lock,queue_key);
    }
    if(submit_result<0){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_BUSY,ptr[3],NRF_BOOTSTRAP_QUEUE_DEPTH);publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_INTERNAL_ERROR);transaction_result=submit_result;}

record_transaction:
    record_twis_transaction(
        transaction_index,
        NRF_BOOT_TWIS_DIRECTION_WRITE_RECEIVED,
        ptr,
        len,
        transaction_result);
}

static int target_read_requested(struct i2c_target_config *config,uint8_t **ptr,uint32_t *len)
{
    nrf_upstream_status_t status;
    nrf_upstream_sensor_snapshot_t sensor;
    nrf_cs_control_result_t cs_result;
    nrf_upstream_result_t result;
    uint32_t transaction_index;
    uint8_t *response = NULL;
    uint32_t response_length = 0U;
    int callback_result = 0;
    k_spinlock_key_t key;
    bool have_cs;
    bool served_window = false;
    (void)config;
    transaction_index = next_twis_transaction_index();
    increment_saturating(&s_twis_read_requested_count);
    record_first_twis_transaction(NRF_BOOTSTRAP_TWIS_TRANSACTION_READ_REQUESTED);
    if(ptr==NULL||len==NULL){callback_result=-EINVAL;goto record_transaction;}

    {
        /* A window request armed the next read: continue the response the master
         * selected with its first read instead of building a new one. */
        const uint8_t *window_data=NULL;
        uint32_t window_length=0U;
        if(nrf_read_window_target_serve(&s_read_window,&window_data,&window_length)){
            response=(uint8_t *)window_data;response_length=window_length;
            *ptr=response;*len=response_length;
            served_window=true;
            goto record_transaction;
        }
    }

    if(s_log_response_selected){*ptr=s_log_response_frame;*len=s_log_response_length;response=s_log_response_frame;response_length=s_log_response_length;goto record_transaction;}

    if(s_cycle_reply_selected){*ptr=s_cycle_reply;*len=s_cycle_reply_length;response=s_cycle_reply;response_length=s_cycle_reply_length;goto record_transaction;}
    {
        /* While a cycle runs or waits to be collected, STATUS reads answer with the cycle status. */
        nrf_cs_cycle_status_t cycle_status;
        bool cycle_reply_active;
        k_spinlock_key_t cycle_key=k_spin_lock(&s_cycle_lock);
        cycle_reply_active=nrf_cs_cycle_runtime_reply_active(&s_cycle);
        if(cycle_reply_active)nrf_cs_cycle_runtime_status(&s_cycle,0U,&cycle_status);
        k_spin_unlock(&s_cycle_lock,cycle_key);
        if(cycle_reply_active&&nrf_cs_cycle_encode_status(&cycle_status,s_cycle_status_frame)==0){
            *ptr=s_cycle_status_frame;*len=NRF_CS_CYCLE_STATUS_LENGTH;response=s_cycle_status_frame;response_length=NRF_CS_CYCLE_STATUS_LENGTH;goto record_transaction;
        }
    }

    key=k_spin_lock(&s_cs_lock);
    have_cs=nrf_cs_control_runtime_take_selected(&s_cs_control,&cs_result);
    k_spin_unlock(&s_cs_lock,key);
    if(have_cs){if(nrf_cs_control_encode_result(&cs_result,s_read_frame)!=0){callback_result=-EIO;goto record_transaction;}*ptr=s_read_frame;*len=NRF_CS_CONTROL_RESULT_LENGTH;response=s_read_frame;response_length=NRF_CS_CONTROL_RESULT_LENGTH;goto record_transaction;}

    key=k_spin_lock(&s_status_lock);status=s_published_status;k_spin_unlock(&s_status_lock,key);
    if(status.role==NRF_UPSTREAM_ROLE_REMOTE&&nrf_sensor_upstream_runtime_zephyr_get_snapshot(&sensor)){
        result=nrf_upstream_protocol_encode_remote_snapshot(&status,&sensor,s_read_frame);if(result!=NRF_UPSTREAM_RESULT_OK){callback_result=-EIO;goto record_transaction;}*ptr=s_read_frame;*len=NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH;response=s_read_frame;response_length=NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH;goto record_transaction;
    }
    result=nrf_upstream_protocol_encode_status(&status,s_read_frame);if(result!=NRF_UPSTREAM_RESULT_OK){callback_result=-EIO;goto record_transaction;}*ptr=s_read_frame;*len=NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH;response=s_read_frame;response_length=NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH;

record_transaction:
    if(!served_window){
        /* Keep what the master was just given so the rest of it can be windowed. */
        nrf_read_window_target_record(&s_read_window,callback_result==0?response:NULL,response_length);
    }
    record_twis_transaction(
        transaction_index,
        NRF_BOOT_TWIS_DIRECTION_READ_REQUESTED,
        response,
        response_length,
        callback_result);
    /*
     * The master never reads more than one window per read (the rest comes with READ_WINDOW), so the TWIS
     * is handed no more than that. With transmit data left over at the master's NACK, MAIN held SDA low in
     * that NACK slot on 15 of 19 reads of the 38 byte LOG STATUS (2026-10-27 capture v5), while every 28
     * byte reply of exactly 28 bytes was clean (243/243). The full response stays recorded for the windows.
     */
    if(callback_result==0&&*len>NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES)*len=NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
    return callback_result;
}

static const struct i2c_target_callbacks s_target_callbacks={.buf_write_received=target_write_received,.buf_read_requested=target_read_requested};

int nrf_bootstrap_runtime_zephyr_start(nrf_node_state_t *node_state)
{
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX]={0};
    size_t hardware_id_length=0U;
    nrf_upstream_status_t initial_status;
    const nrf_bootstrap_ops_t ops={.readdress=runtime_readdress,.target_is_registered=runtime_target_is_registered,.target_address=runtime_target_address,.cold_reboot=runtime_cold_reboot,.context=NULL};
    int result;
    if(node_state==NULL)return -EINVAL;
    if(s_started)return -EBUSY;
    nrf_hci_workqueue_start();
    result=nrf_log_runtime_zephyr_init();if(result!=0)return result;
    result=nrf_upstream_target_zephyr_read_device_id(hardware_id,sizeof(hardware_id),&hardware_id_length);if(result!=0)return result;
    if(nrf_bootstrap_init(&s_bootstrap,node_state,hardware_id,hardware_id_length,&ops)!=NRF_UPSTREAM_RESULT_OK)return -EINVAL;
    if(nrf_cs_control_runtime_init(&s_cs_control)!=0)return -EINVAL;
    nrf_read_window_target_init(&s_read_window);
    {
        const nrf_cs_cycle_runtime_ops_t cycle_ops={.now_ms=cycle_now_ms,.abort_attempt=cycle_abort_attempt};
        k_spinlock_key_t cycle_key=k_spin_lock(&s_cycle_lock);
        nrf_cs_cycle_runtime_init(&s_cycle,&cycle_ops);
        k_spin_unlock(&s_cycle_lock,cycle_key);
        s_cycle_reply_selected=false;
    }
    k_msgq_purge(&s_assign_queue);k_msgq_purge(&s_cs_execute_queue);s_assign_sequence=0U;s_cs_execute_sequence=0U;k_work_init(&s_assign_work,assign_work_handler);k_work_init(&s_cs_execute_work,cs_execute_work_handler);
    if(nrf_bootstrap_make_status(&s_bootstrap,&initial_status)!=NRF_UPSTREAM_RESULT_OK)return -EIO;
    initial_status.target_address=NRF_UPSTREAM_ADDRESS_DISCOVERY;publish_status(&initial_status);
    result=nrf_upstream_target_zephyr_init(&s_target_callbacks);if(result!=0)return result;
    s_started=true;result=nrf_upstream_target_zephyr_register(NRF_UPSTREAM_ADDRESS_DISCOVERY);if(result!=0){s_started=false;return result;}
    publish_core_status();return 0;
}

#if defined(NRF_BOOTSTRAP_RUNTIME_TEST_HOOKS)
void nrf_bootstrap_runtime_zephyr_test_forget_started(void)
{
    s_started = false;
}
#endif
