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
#include "nrf_cs_control_protocol.h"
#include "nrf_cs_control_runtime.h"
#include "nrf_cs_pair_runtime_zephyr.h"
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
static bool s_started;

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
        if(nrf_cs_pair_runtime_zephyr_execute(&request.step,cs_runtime_complete,NULL)!=0){
            key=k_spin_lock(&s_cs_lock);
            (void)nrf_cs_control_runtime_complete(&s_cs_control,&request.step,&failure);
            k_spin_unlock(&s_cs_lock,key);
        }
    }
}

static void assign_work_handler(struct k_work *work)
{
    nrf_upstream_assign_t assign;
    (void)work;
    while(k_msgq_get(&s_assign_queue,&assign,K_NO_WAIT)==0){
        nrf_upstream_result_t result;
        publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_PENDING);
        result=nrf_bootstrap_process_assign(&s_bootstrap,&assign);
        if(result==NRF_UPSTREAM_RESULT_OK&&assign.role==NRF_UPSTREAM_ROLE_REMOTE){if(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign)!=0){assignment_runtime_failure();continue;}}
        if(result==NRF_UPSTREAM_RESULT_OK&&nrf_cs_pair_runtime_zephyr_set_node_id(assign.node_id)!=0){assignment_runtime_failure();continue;}
        if(result==NRF_UPSTREAM_RESULT_OK){k_spinlock_key_t key=k_spin_lock(&s_cs_lock);int cs_result=nrf_cs_control_runtime_assign_node(&s_cs_control,assign.node_id);k_spin_unlock(&s_cs_lock,key);if(cs_result!=0){assignment_runtime_failure();continue;}}
        publish_core_status();
    }
}

static void target_write_received(struct i2c_target_config *config,uint8_t *ptr,uint32_t len)
{
    nrf_upstream_assign_t assign;
    nrf_upstream_sensor_refresh_t sensor_request;
    nrf_upstream_height_reference_t height_reference_request;
    nrf_cs_control_request_t cs_request;
    nrf_upstream_result_t result;
    uint32_t request_id;
    int submit_result;
    (void)config;

    if((size_t)len==NRF_CS_CONTROL_EXECUTE_LENGTH&&ptr!=NULL&&ptr[3]==NRF_CS_CONTROL_MESSAGE_EXECUTE){
        if(nrf_cs_control_decode_execute(ptr,(size_t)len,&cs_request)!=0)return;
        if(k_msgq_put(&s_cs_execute_queue,&cs_request,K_NO_WAIT)!=0)return;
        (void)k_work_submit(&s_cs_execute_work);
        return;
    }
    if((size_t)len==NRF_CS_CONTROL_STATUS_REQUEST_LENGTH&&ptr!=NULL&&ptr[3]==NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST){
        if(nrf_cs_control_decode_status_request(ptr,(size_t)len,&cs_request)==0){k_spinlock_key_t key=k_spin_lock(&s_cs_lock);(void)nrf_cs_control_runtime_select_result(&s_cs_control,&cs_request);k_spin_unlock(&s_cs_lock,key);}return;
    }
    if((size_t)len==NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH){
        result=nrf_upstream_protocol_decode_sensor_refresh(ptr,(size_t)len,&sensor_request);
        if(result==NRF_UPSTREAM_RESULT_OK)(void)nrf_sensor_upstream_runtime_zephyr_request_refresh(&sensor_request);
        return;
    }
    if((size_t)len==NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH&&ptr!=NULL&&ptr[3]==NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE){
        result=nrf_upstream_protocol_decode_height_reference(ptr,(size_t)len,&height_reference_request);
        if(result==NRF_UPSTREAM_RESULT_OK)(void)nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height_reference_request);
        return;
    }
    request_id=nrf_upstream_protocol_request_id_hint(ptr,(size_t)len);
    result=nrf_upstream_protocol_decode_assign(ptr,(size_t)len,&assign);
    if(result!=NRF_UPSTREAM_RESULT_OK){publish_result_context(request_id,result);return;}
    if(k_msgq_put(&s_assign_queue,&assign,K_NO_WAIT)!=0){publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_BUSY);return;}
    submit_result=k_work_submit(&s_assign_work);
    if(submit_result<0)publish_result_context(assign.request_id,NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
}

static int target_read_requested(struct i2c_target_config *config,uint8_t **ptr,uint32_t *len)
{
    nrf_upstream_status_t status;
    nrf_upstream_sensor_snapshot_t sensor;
    nrf_cs_control_result_t cs_result;
    nrf_upstream_result_t result;
    k_spinlock_key_t key;
    bool have_cs;
    (void)config;
    if(ptr==NULL||len==NULL)return -EINVAL;

    key=k_spin_lock(&s_cs_lock);
    have_cs=nrf_cs_control_runtime_take_selected(&s_cs_control,&cs_result);
    k_spin_unlock(&s_cs_lock,key);
    if(have_cs){if(nrf_cs_control_encode_result(&cs_result,s_read_frame)!=0)return -EIO;*ptr=s_read_frame;*len=NRF_CS_CONTROL_RESULT_LENGTH;return 0;}

    key=k_spin_lock(&s_status_lock);status=s_published_status;k_spin_unlock(&s_status_lock,key);
    if(status.role==NRF_UPSTREAM_ROLE_REMOTE&&nrf_sensor_upstream_runtime_zephyr_get_snapshot(&sensor)){
        result=nrf_upstream_protocol_encode_remote_snapshot(&status,&sensor,s_read_frame);if(result!=NRF_UPSTREAM_RESULT_OK)return -EIO;*ptr=s_read_frame;*len=NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH;return 0;
    }
    result=nrf_upstream_protocol_encode_status(&status,s_read_frame);if(result!=NRF_UPSTREAM_RESULT_OK)return -EIO;*ptr=s_read_frame;*len=NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH;return 0;
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
