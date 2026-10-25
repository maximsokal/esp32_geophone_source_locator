#include "sensor_bus_lines_fake.h"
#include "sensor_bus_lines.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

fake_lines_t g_fake_lines;

esp_err_t i2c_new_master_bus(
    const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus)
{
    assert(config != NULL && bus != NULL);
    assert(config->i2c_port == I2C_NUM_0);
    assert(config->sda_io_num == BOARD_SENSOR_I2C_SDA_GPIO);
    assert(config->scl_io_num == BOARD_SENSOR_I2C_SCL_GPIO);
    assert(config->clk_source == I2C_CLK_SRC_DEFAULT);
    assert(config->glitch_ignore_cnt == 7U);
    assert(config->flags.enable_internal_pullup);
    ++g_fake_lines.controller_create_calls;
    if (g_fake_lines.controller_create_result != ESP_OK) {
        return g_fake_lines.controller_create_result;
    }
    *bus = &g_fake_lines;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus == &g_fake_lines);
    ++g_fake_lines.controller_delete_calls;
    return g_fake_lines.controller_delete_result;
}

void fake_lines_reset(void)
{
    memset(&g_fake_lines, 0, sizeof(g_fake_lines));
    atomic_init(&g_fake_lines.now_us, INT64_C(1000000));
    g_fake_lines.ctrl_sda_low = false;
    g_fake_lines.ctrl_scl_low = false;
}

static bool sda_held(void)
{
    return g_fake_lines.sda_hold_clocks < 0 ||
        g_fake_lines.rising_edges < g_fake_lines.sda_hold_clocks;
}

static bool line_scl(void)
{
    return !g_fake_lines.ctrl_scl_low && !g_fake_lines.scl_held;
}

static bool line_sda(void)
{
    return !g_fake_lines.ctrl_sda_low && !sda_held();
}

static void observe(void)
{
    const bool scl = line_scl();
    if (scl && g_fake_lines.last_scl_low) {
        ++g_fake_lines.rising_edges;
    }
    g_fake_lines.last_scl_low = !scl;
}

static bool read_sda(void *context) { (void)context; return line_sda(); }
static bool read_scl(void *context) { (void)context; return line_scl(); }

static void set_sda(void *context, bool released)
{
    (void)context;
    g_fake_lines.ctrl_sda_low = !released;
    observe();
}

static void set_scl(void *context, bool released)
{
    (void)context;
    if (!released) {
        assert(!g_fake_lines.scl_held); /* never clock a held SCL */
        ++g_fake_lines.scl_driven_low;
    }
    g_fake_lines.ctrl_scl_low = !released;
    observe();
}

static void delay(void *context, uint32_t us)
{
    (void)context;
    g_fake_lines.now_us += us;
    observe();
}

esp_err_t sensor_bus_lines_bitbang_io(i2c_line_io_t *io)
{
    ++g_fake_lines.bitbang_prepare_calls;
    g_fake_lines.ctrl_sda_low = false;
    g_fake_lines.ctrl_scl_low = false;
    g_fake_lines.last_scl_low = !line_scl();
    io->read_sda = read_sda;
    io->read_scl = read_scl;
    io->set_sda = set_sda;
    io->set_scl = set_scl;
    io->delay_us = delay;
    io->context = NULL;
    return ESP_OK;
}

void sensor_bus_lines_read(bool *sda_high, bool *scl_high)
{
    *sda_high = line_sda();
    *scl_high = line_scl();
}

esp_err_t sensor_bus_lines_controller_reset(i2c_bus_handle_t bus)
{
    assert(bus != NULL);
    assert(!g_fake_lines.scl_held);
    ++g_fake_lines.controller_reset_calls;
    if (g_fake_lines.controller_reset_fails) {
        return ESP_FAIL;
    }
    if (g_fake_lines.controller_reset_clears_sda) {
        g_fake_lines.sda_hold_clocks = 0;
    }
    return ESP_OK;
}

esp_err_t sensor_bus_lines_probe(i2c_bus_handle_t bus, uint8_t address)
{
    assert(bus != NULL);
    assert(address < 128U);
    ++g_fake_lines.probe_calls;
    /* Unconfigured: nobody answers the probe. */
    return g_fake_lines.probe_configured ? g_fake_lines.probe_result[address] : ESP_ERR_NOT_FOUND;
}

void sensor_bus_lines_delay_us(uint32_t us)
{
    g_fake_lines.now_us += us;
}

int64_t sensor_bus_lines_now_us(void)
{
    return atomic_load(&g_fake_lines.now_us);
}

void fake_sensor_bus_log(const char *tag, const char *format, ...)
{
    (void)tag;
    ++g_fake_lines.log_calls;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(g_fake_lines.last_log, sizeof(g_fake_lines.last_log), format, args);
    va_end(args);
    if (g_fake_lines.log_observer != NULL) {
        g_fake_lines.log_observer(g_fake_lines.last_log);
    }
    if (strstr(g_fake_lines.last_log, "I2C_TIMEOUT_CONFIG") != NULL) {
        (void)strncpy(g_fake_lines.timeout_config_log, g_fake_lines.last_log,
                      sizeof(g_fake_lines.timeout_config_log) - 1U);
        g_fake_lines.timeout_config_log[sizeof(g_fake_lines.timeout_config_log) - 1U] = '\0';
    }
    if (strstr(g_fake_lines.last_log, "I2C_INIT_LINES") != NULL) {
        (void)strncpy(g_fake_lines.init_lines_log, g_fake_lines.last_log,
                      sizeof(g_fake_lines.init_lines_log) - 1U);
        g_fake_lines.init_lines_log[sizeof(g_fake_lines.init_lines_log) - 1U] = '\0';
    }
    if (strstr(g_fake_lines.last_log, "TCA_HOLD_BUDGET_EXCEEDED") != NULL) {
        g_fake_lines.hold_budget_exceeded_logged = true;
    }
}
