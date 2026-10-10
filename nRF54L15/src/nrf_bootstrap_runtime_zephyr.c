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
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"
#include "nrf_upstream_target_zephyr.h"

#define NRF_BOOTSTRAP_QUEUE_DEPTH 4U
#define NRF_CS_EXECUTE_QUEUE_DEPTH 2U

K_MSGQ_DEFINE(s_assign_queue, sizeof(nrf_upstream_assign_t), NRF_BOOTSTRAP_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(s_cs_execute_queue, sizeof(nrf_cs_control_request_t), NRF_CS_EXECUTE_QUEUE_DEPTH, 4);

static struct k_work s_assign_work;
static struct k_work s_cs_execute_work;
static struct k_spinlock s_status_lock;
static struct k_spinlock s_cs_lock;
static nrf_bootstrap_t s_bootstrap;
static nrf_upstream_status_t s_published_status;
static nrf_cs_control_runtime_t s_cs_control;
static uint8_t s_read_frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
static uint8_t s_log_response_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
static uint32_t s_log_response_length;
static bool s_log_response_selected;
static bool s_started;
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
    nrf_cs_control_request_t request;
    (void)work;
    while(k_msgq_get(&s_cs_execute_queue,&request,K_NO_WAIT)==0){
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
    nrf_upstream_assign_t assign;
    nrf_upstream_status_t previous_status;
    nrf_upstream_status_t current_status;
    (void)work;
    while(k_msgq_get(&s_assign_queue,&assign,K_NO_WAIT)==0){
        nrf_upstream_result_t result;
        k_spinlock_key_t status_key=k_spin_lock(&s_status_lock);
        previous_status=s_published_status;
        k_spin_unlock(&s_status_lock,status_key);
        publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_PENDING);
        result=nrf_bootstrap_process_assign(&s_bootstrap,&assign);
        if(result==NRF_UPSTREAM_RESULT_OK&&assign.role==NRF_UPSTREAM_ROLE_REMOTE){if(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign)!=0){assignment_runtime_failure();continue;}}
        if(result==NRF_UPSTREAM_RESULT_OK&&nrf_cs_pair_runtime_zephyr_set_node_id(assign.node_id)!=0){assignment_runtime_failure();continue;}
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
       (frame[3]!=NRF_LOG_MESSAGE_STATUS_REQUEST&&frame[3]!=NRF_LOG_MESSAGE_READ_REQUEST)){
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
    int submit_result;
    int32_t transaction_result = 0;
    (void)config;

    transaction_index = next_twis_transaction_index();
    increment_saturating(&s_twis_write_received_count);
    add_saturating(&s_twis_write_received_bytes, len);
    record_first_twis_transaction(NRF_BOOTSTRAP_TWIS_TRANSACTION_WRITE_RECEIVED);
    record_twis_request_type(ptr, len);

    s_log_response_selected=false;
    if (len == 0U) {
        goto record_transaction;
    }

    if(handle_log_write(ptr,(size_t)len,&transaction_result))goto record_transaction;

    if((size_t)len==NRF_CS_CONTROL_EXECUTE_LENGTH&&ptr!=NULL&&ptr[3]==NRF_CS_CONTROL_MESSAGE_EXECUTE){
        transaction_result = nrf_cs_control_decode_execute(
            ptr, (size_t)len, &cs_request);
        if(transaction_result!=0){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],1U);goto record_transaction;}
        if(k_msgq_put(&s_cs_execute_queue,&cs_request,K_NO_WAIT)!=0){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_BUSY,ptr[3],NRF_CS_EXECUTE_QUEUE_DEPTH);transaction_result=-EBUSY;goto record_transaction;}
        submit_result = k_work_submit(&s_cs_execute_work);
        transaction_result = submit_result < 0 ? submit_result : 0;
        goto record_transaction;
    }
    if((size_t)len==NRF_CS_CONTROL_STATUS_REQUEST_LENGTH&&ptr!=NULL&&ptr[3]==NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST){
        transaction_result = nrf_cs_control_decode_status_request(
            ptr, (size_t)len, &cs_request);
        if(transaction_result==0){k_spinlock_key_t key=k_spin_lock(&s_cs_lock);transaction_result=nrf_cs_control_runtime_select_result(&s_cs_control,&cs_request);k_spin_unlock(&s_cs_lock,key);}else{nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],1U);}
        goto record_transaction;
    }
    if((size_t)len==NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH){
        result=nrf_upstream_protocol_decode_sensor_refresh(ptr,(size_t)len,&sensor_request);
        if(result==NRF_UPSTREAM_RESULT_OK)transaction_result=nrf_sensor_upstream_runtime_zephyr_request_refresh(&sensor_request);
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr!=NULL&&len>3U?ptr[3]:0U,(uint32_t)result);
        if(result!=NRF_UPSTREAM_RESULT_OK)transaction_result=(int32_t)result;
        goto record_transaction;
    }
    if((size_t)len==NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH&&ptr!=NULL&&ptr[3]==NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE){
        result=nrf_upstream_protocol_decode_height_reference(ptr,(size_t)len,&height_reference_request);
        if(result==NRF_UPSTREAM_RESULT_OK)transaction_result=nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height_reference_request);
        else nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME,ptr[3],(uint32_t)result);
        if(result!=NRF_UPSTREAM_RESULT_OK)transaction_result=(int32_t)result;
        goto record_transaction;
    }
    if((size_t)len==NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH&&ptr!=NULL&&ptr[3]==NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE){
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
    if(k_msgq_put(&s_assign_queue,&assign,K_NO_WAIT)!=0){nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_TWIS_BUSY,ptr[3],NRF_BOOTSTRAP_QUEUE_DEPTH);publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_BUSY);transaction_result=(int32_t)NRF_UPSTREAM_RESULT_BUSY;goto record_transaction;}
    submit_result=k_work_submit(&s_assign_work);
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
    (void)config;
    transaction_index = next_twis_transaction_index();
    increment_saturating(&s_twis_read_requested_count);
    record_first_twis_transaction(NRF_BOOTSTRAP_TWIS_TRANSACTION_READ_REQUESTED);
    if(ptr==NULL||len==NULL){callback_result=-EINVAL;goto record_transaction;}

    if(s_log_response_selected){*ptr=s_log_response_frame;*len=s_log_response_length;response=s_log_response_frame;response_length=s_log_response_length;goto record_transaction;}

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
    record_twis_transaction(
        transaction_index,
        NRF_BOOT_TWIS_DIRECTION_READ_REQUESTED,
        response,
        response_length,
        callback_result);
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
    result=nrf_log_runtime_zephyr_init();if(result!=0)return result;
    result=nrf_upstream_target_zephyr_read_device_id(hardware_id,sizeof(hardware_id),&hardware_id_length);if(result!=0)return result;
    if(nrf_bootstrap_init(&s_bootstrap,node_state,hardware_id,hardware_id_length,&ops)!=NRF_UPSTREAM_RESULT_OK)return -EINVAL;
    if(nrf_cs_control_runtime_init(&s_cs_control)!=0)return -EINVAL;
    k_msgq_purge(&s_assign_queue);k_msgq_purge(&s_cs_execute_queue);k_work_init(&s_assign_work,assign_work_handler);k_work_init(&s_cs_execute_work,cs_execute_work_handler);
    if(nrf_bootstrap_make_status(&s_bootstrap,&initial_status)!=NRF_UPSTREAM_RESULT_OK)return -EIO;
    initial_status.target_address=NRF_UPSTREAM_ADDRESS_DISCOVERY;publish_status(&initial_status);
    result=nrf_upstream_target_zephyr_init(&s_target_callbacks);if(result!=0)return result;
    s_started=true;result=nrf_upstream_target_zephyr_register(NRF_UPSTREAM_ADDRESS_DISCOVERY);if(result!=0){s_started=false;return result;}
    publish_core_status();return 0;
}
