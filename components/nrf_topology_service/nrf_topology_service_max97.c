/* MAX-97 extends the existing topology worker without creating a second TCA owner. */
#define service_worker service_worker_pre_max97
#define nrf_topology_service_start nrf_topology_service_start_pre_max97
#include "nrf_topology_service.c"
#undef nrf_topology_service_start
#undef service_worker

static void (*const s_pre_max97_worker_reference)(void *) __attribute__((unused)) = service_worker_pre_max97;

#include "nrf_cs_authority.h"
#include "nrf_cs_control_protocol.h"

#define NRF_CS_PUMP_INTERVAL_MS 10U

typedef struct {
    nrf_topology_platform_t *platform;
    nrf_topology_snapshot_t *snapshot;
} nrf_cs_platform_context_t;

static bool s_cs_request_pending;
static bool s_cs_snapshot_valid;
static nrf_cs_policy_t s_cs_requested_policy;
static nrf_cs_cycle_snapshot_t s_cs_cycle_snapshot;

static bool cs_node_present(const nrf_topology_snapshot_t *snapshot, nrf_cs_node_id_t node)
{
    if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED && snapshot->main.node_id == node) return true;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED && snapshot->remotes[channel].node_id == node) return true;
    }
    return false;
}

static size_t cs_collect_nodes(const nrf_topology_snapshot_t *snapshot, nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES], bool *overflow)
{
    size_t count = 0U;
    *overflow = false;
    if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) nodes[count++] = snapshot->main.node_id;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (snapshot->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        if (count >= NRF_CS_MAX_NODES) { *overflow = true; continue; }
        nodes[count++] = snapshot->remotes[channel].node_id;
    }
    return count;
}

static esp_err_t cs_route_open(nrf_cs_platform_context_t *ctx, nrf_cs_node_id_t node, i2c_bus_device_handle_t *device, bool *remote)
{
    esp_err_t err = platform_open_address_handles(ctx->platform);
    if (err != ESP_OK) return err;
    if (ctx->snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED && ctx->snapshot->main.node_id == node) {
        if (ctx->platform->mux_bound) (void)platform_mux_close(ctx->platform);
        *device = ctx->platform->main_device; *remote = false; return ESP_OK;
    }
    if (!ctx->snapshot->tca_available || ctx->snapshot->tca_fault || !ctx->platform->mux_bound) return ESP_ERR_NOT_FOUND;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (ctx->snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED && ctx->snapshot->remotes[channel].node_id == node) {
            err = platform_mux_select(ctx->platform, channel);
            if (err != ESP_OK) return err;
            *device = ctx->platform->remote_device; *remote = true; return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void cs_route_close(nrf_cs_platform_context_t *ctx, bool remote)
{
    if (remote && ctx->platform->mux_bound && platform_mux_close(ctx->platform) != ESP_OK) service_request_topology_refresh_from_worker();
}

static int cs_write_execute(void *context, const nrf_cs_control_request_t *request)
{
    nrf_cs_platform_context_t *ctx = context;
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    i2c_bus_device_handle_t device = NULL;
    bool remote = false;
    esp_err_t err;
    if (nrf_cs_control_encode_execute(request, frame) != 0) return -1;
    err = cs_route_open(ctx, request->step.initiator, &device, &remote);
    if (err == ESP_OK) err = i2c_bus_write_bytes(device, NULL_I2C_MEM_ADDR, sizeof(frame), frame);
    cs_route_close(ctx, remote);
    if (err != ESP_OK) { service_request_topology_refresh_from_worker(); return -1; }
    return 0;
}

static int cs_poll_result(void *context, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *result)
{
    nrf_cs_platform_context_t *ctx = context;
    uint8_t selector[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
    i2c_bus_device_handle_t device = NULL;
    bool remote = false;
    esp_err_t err;
    if (nrf_cs_control_encode_status_request(request, selector) != 0) return -1;
    err = cs_route_open(ctx, request->step.initiator, &device, &remote);
    if (err == ESP_OK) err = i2c_bus_write_bytes(device, NULL_I2C_MEM_ADDR, sizeof(selector), selector);
    if (err == ESP_OK) err = i2c_bus_read_bytes(device, NULL_I2C_MEM_ADDR, sizeof(frame), frame);
    cs_route_close(ctx, remote);
    if (err != ESP_OK) { service_request_topology_refresh_from_worker(); return -1; }
    return nrf_cs_control_decode_result(frame, sizeof(frame), result) == 0 ? 0 : -1;
}

static uint64_t cs_now_ms(void *context){return platform_now_ms(((nrf_cs_platform_context_t *)context)->platform);}

static void cs_publish(const nrf_cs_authority_t *authority, bool active, int result)
{
    nrf_cs_cycle_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.active = active;
    snapshot.complete = !active && nrf_cs_scheduler_is_complete(&authority->scheduler);
    snapshot.result = result;
    snapshot.cycle_id = authority->scheduler.cycle_id;
    snapshot.membership_generation = authority->scheduler.membership_generation;
    snapshot.pair_count = nrf_cs_authority_pair_count(authority);
    for (size_t i = 0U; i < snapshot.pair_count && i < NRF_CS_MAX_PAIRS; ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_authority_pair_at(authority, i);
        if (pair != NULL) snapshot.pairs[i] = *pair;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_cs_cycle_snapshot = snapshot;
    s_cs_snapshot_valid = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool cs_take_request(nrf_cs_policy_t *policy)
{
    bool pending;
    portENTER_CRITICAL(&s_state_lock);
    pending = s_cs_request_pending;
    if (pending) { *policy = s_cs_requested_policy; s_cs_request_pending = false; }
    portEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static void cs_sync_disconnects(nrf_cs_authority_t *authority, const nrf_topology_snapshot_t *snapshot)
{
    for (size_t i = 0U; i < authority->scheduler.node_count; ++i) {
        const nrf_cs_node_id_t node = authority->scheduler.nodes[i];
        if (!cs_node_present(snapshot, node)) (void)nrf_cs_authority_disconnect(authority, node);
    }
}

static void service_worker(void *context)
{
    (void)context;
    nrf_topology_platform_t platform;
    nrf_topology_controller_t controller;
    nrf_sensor_transport_controller_t sensor_controller;
    nrf_topology_snapshot_t snapshot;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_cs_authority_t authority;
    nrf_cs_platform_context_t cs_context = { .platform = &platform, .snapshot = &snapshot };
    nrf_cs_authority_ops_t cs_ops = { .context = &cs_context, .write_execute = cs_write_execute, .poll_result = cs_poll_result, .now_ms = cs_now_ms };
    const nrf_topology_config_t topology_config = { NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS, NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS, 0U };
    const nrf_sensor_transport_config_t sensor_config = { NRF_SENSOR_TRANSPORT_DEFAULT_TIMEOUT_MS, NRF_SENSOR_TRANSPORT_DEFAULT_POLL_MS, 0U };
    const TickType_t reconcile_period_ticks = pdMS_TO_TICKS(NRF_TOPOLOGY_RECONCILE_PERIOD_MS);
    TickType_t last_reconcile_tick = xTaskGetTickCount();
    bool first_reconcile = true;
    bool controllers_ready = false;
    bool cs_initialized = false;
    bool stop_before_start = service_register_current_task();
    memset(&platform,0,sizeof(platform)); memset(&snapshot,0,sizeof(snapshot)); memset(cache,0,sizeof(cache)); memset(&authority,0,sizeof(authority));

    if (!stop_before_start) {
        nrf_topology_config_t tc = topology_config; nrf_sensor_transport_config_t sc = sensor_config;
        tc.request_seed = esp_random(); sc.request_seed = esp_random(); if (tc.request_seed==0U) tc.request_seed=1U; if (sc.request_seed==0U) sc.request_seed=1U;
        controllers_ready = nrf_topology_controller_init(&controller,&tc)==ESP_OK && nrf_sensor_transport_controller_init(&sensor_controller,&sc)==ESP_OK;
    }

    while (!stop_before_start && controllers_ready && !service_stop_requested()) {
        const TickType_t now = xTaskGetTickCount();
        const bool explicit_refresh = service_take_topology_refresh_request();
        if (first_reconcile || explicit_refresh || tick_period_due(now,last_reconcile_tick,reconcile_period_ticks)) {
            (void)service_run_topology_reconcile(&platform,&controller,&snapshot,cache);
            last_reconcile_tick=xTaskGetTickCount(); first_reconcile=false;
            if (cs_initialized && nrf_cs_authority_is_active(&authority)) cs_sync_disconnects(&authority,&snapshot);
        }
        if (service_stop_requested()) break;

        nrf_cs_policy_t requested_policy;
        if (cs_take_request(&requested_policy)) {
            nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES]; bool overflow=false; size_t count=cs_collect_nodes(&snapshot,nodes,&overflow);
            int start_result = NRF_CS_AUTHORITY_ERR_STATE;
            if (!overflow && count >= 2U) {
                if (!cs_initialized) { start_result=nrf_cs_authority_init(&authority,&requested_policy,&cs_ops,esp_random()); cs_initialized=start_result==NRF_CS_AUTHORITY_OK; }
                else if (!nrf_cs_authority_is_active(&authority)) authority.scheduler.policy=requested_policy;
                if (cs_initialized) start_result=nrf_cs_authority_start_cycle(&authority,nodes,count);
            }
            if (!cs_initialized || start_result < 0) { portENTER_CRITICAL(&s_state_lock); memset(&s_cs_cycle_snapshot,0,sizeof(s_cs_cycle_snapshot)); s_cs_cycle_snapshot.result=start_result; s_cs_snapshot_valid=true; portEXIT_CRITICAL(&s_state_lock); }
            else cs_publish(&authority,true,start_result);
        }

        if (service_stop_requested()) break;
        if (!cs_initialized || !nrf_cs_authority_is_active(&authority) || !authority.command_active) {
            const uint8_t sensor_mask=service_take_sensor_mask();
            if(sensor_mask!=0U)service_process_sensor_mask(&platform,&sensor_controller,&snapshot,cache,sensor_mask,last_reconcile_tick,reconcile_period_ticks);
        }
        if (service_stop_requested()) break;

        if (cs_initialized && nrf_cs_authority_is_active(&authority)) {
            const nrf_cs_authority_result_t cs_result=nrf_cs_authority_pump(&authority);
            cs_publish(&authority,nrf_cs_authority_is_active(&authority),cs_result);
        }

        TickType_t wait_ticks=0U;
        if (!service_has_immediate_work()) {
            if (cs_initialized && nrf_cs_authority_is_active(&authority)) wait_ticks=pdMS_TO_TICKS(NRF_CS_PUMP_INTERVAL_MS);
            else wait_ticks=tick_wait_until_period(xTaskGetTickCount(),last_reconcile_tick,reconcile_period_ticks);
        }
        (void)ulTaskNotifyTake(pdTRUE,wait_ticks);
    }
    platform_close(&platform); service_clear_current_task(); vTaskDelete(NULL);
}

esp_err_t nrf_topology_service_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if(s_stop_requested){portEXIT_CRITICAL(&s_state_lock);return ESP_ERR_INVALID_STATE;}
    if(s_task!=NULL||s_starting){portEXIT_CRITICAL(&s_state_lock);return ESP_OK;}
    s_starting=true; s_snapshot_valid=false; s_sensor_cache_valid=false; s_topology_refresh_requested=false; s_sensor_pending_mask=0U; s_sensor_inflight_mask=0U; s_cs_request_pending=false; s_cs_snapshot_valid=false;
    memset(&s_snapshot,0,sizeof(s_snapshot)); memset(s_sensor_cache,0,sizeof(s_sensor_cache)); memset(&s_cs_cycle_snapshot,0,sizeof(s_cs_cycle_snapshot)); portEXIT_CRITICAL(&s_state_lock);
    TaskHandle_t task=NULL; const BaseType_t result=xTaskCreatePinnedToCore(service_worker,"nrf_topology",APP_TASK_STACK_NRF_TOPOLOGY,NULL,APP_TASK_PRIORITY_NRF_TOPOLOGY,&task,APP_TASK_CORE_SERVICES);
    if(result!=pdPASS){portENTER_CRITICAL(&s_state_lock);s_starting=false;portEXIT_CRITICAL(&s_state_lock);return ESP_ERR_NO_MEM;}
    ESP_LOGI(TAG,"Topology/CS authority service started on core=%d",APP_TASK_CORE_SERVICES); return ESP_OK;
}

esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy)
{
    TaskHandle_t task; bool available;
    if(policy==NULL||policy->pair_timeout_ms==0U||policy->max_attempts==0U||policy->min_confidence>NRF_CS_CONFIDENCE_MAX)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock); task=s_task; available=task!=NULL||s_starting;
    if(available&&!s_cs_request_pending&&(!s_cs_snapshot_valid||!s_cs_cycle_snapshot.active)){s_cs_requested_policy=*policy;s_cs_request_pending=true;}
    else if(available){portEXIT_CRITICAL(&s_state_lock);return ESP_ERR_INVALID_STATE;}
    portEXIT_CRITICAL(&s_state_lock);
    if (!available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (task != NULL) {
        (void)xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot)
{
    bool valid;
    if(snapshot==NULL)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock); valid=s_cs_snapshot_valid; if(valid)*snapshot=s_cs_cycle_snapshot; portEXIT_CRITICAL(&s_state_lock);
    return valid?ESP_OK:ESP_ERR_INVALID_STATE;
}
