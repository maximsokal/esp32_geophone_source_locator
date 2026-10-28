#include "nrf_sensor_bus_zephyr.h"

#include <errno.h>
#include <string.h>

#if !defined(CONFIG_BOARD_NATIVE_SIM) /* native_sim has no Nordic HAL: its fake bus reports no line levels */
#include <hal/nrf_gpio.h>
#define NRF_SENSOR_BUS_CAN_SAMPLE_LINES 1
#endif
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <zephyr/sys/printk.h>
#endif

typedef struct {
    const struct device *device;
} nrf_sensor_bus_zephyr_context_t;

static nrf_sensor_bus_zephyr_context_t s_private_context = {
    .device = DEVICE_DT_GET(DT_NODELABEL(i2c30)),
};

#if defined(NRF_SENSOR_BUS_CAN_SAMPLE_LINES)
/*
 * i2c30 pins, from zephyr/boards/seeed/xiao_nrf54l15/xiao_nrf54l15-pinctrl.dtsi:59-64 (SDA P0.04, SCL P0.03);
 * app.overlay does not override them.
 */
#define NRF_SENSOR_BUS_SDA_PIN NRF_GPIO_PIN_MAP(0, 4)
#define NRF_SENSOR_BUS_SCL_PIN NRF_GPIO_PIN_MAP(0, 3)
#endif

#define NRF_SENSOR_BUS_FAULT_SLOTS 8U

/*
 * Failed transfers by address since the last take. Zephyr reports every address NACK, data NACK and overrun as
 * -EIO, so the console line alone cannot say whether a device is missing or the bus is broken; this keeps the
 * cause and the line levels for the diagnostic event.
 */
static struct {
    struct k_spinlock lock;
    bool used[NRF_SENSOR_BUS_FAULT_SLOTS];
    nrf_sensor_bus_fault_t fault[NRF_SENSOR_BUS_FAULT_SLOTS];
    uint32_t dropped;
} s_faults;

static void nrf_sensor_bus_zephyr_sample_lines(const struct device *device, nrf_sensor_bus_fault_t *fault)
{
#if defined(NRF_SENSOR_BUS_CAN_SAMPLE_LINES)
    /*
     * The driver suspends the controller after every transfer and the sleep pinctrl state disconnects the input
     * buffers, so a plain read now would report both lines low. Resume it for the sample. After a timeout the
     * driver has already run its own bus recovery, so a line still low here is held by a device.
     */
    if (pm_device_runtime_get(device) != 0) {
        return;
    }
    fault->sda_high = nrf_gpio_pin_read(NRF_SENSOR_BUS_SDA_PIN) != 0U;
    fault->scl_high = nrf_gpio_pin_read(NRF_SENSOR_BUS_SCL_PIN) != 0U;
    fault->lines_valid = true;
    (void)pm_device_runtime_put(device);
#else
    (void)device;
    (void)fault;
#endif
}

static nrf_sensor_bus_fault_t nrf_sensor_bus_zephyr_note_fault(
    const struct device *device,
    uint8_t address,
    int transfer_result)
{
    const int64_t magnitude = transfer_result < 0 ? -(int64_t)transfer_result : (int64_t)transfer_result;
    nrf_sensor_bus_fault_t sample = {
        .address = address,
        .cause = nrf_sensor_bus_classify(transfer_result),
        .errno_abs = magnitude > UINT8_MAX ? UINT8_MAX : (uint8_t)magnitude,
        .failures = 1U,
    };
    nrf_sensor_bus_zephyr_sample_lines(device, &sample);

    k_spinlock_key_t key = k_spin_lock(&s_faults.lock);
    size_t free_slot = NRF_SENSOR_BUS_FAULT_SLOTS;
    bool stored = false;
    for (size_t i = 0U; i < NRF_SENSOR_BUS_FAULT_SLOTS && !stored; ++i) {
        if (!s_faults.used[i]) {
            if (free_slot == NRF_SENSOR_BUS_FAULT_SLOTS) free_slot = i;
            continue;
        }
        if (s_faults.fault[i].address != address) continue;
        const uint32_t failures = s_faults.fault[i].failures == UINT32_MAX
            ? UINT32_MAX : s_faults.fault[i].failures + 1U;
        if (nrf_sensor_bus_fault_outranks(&sample, &s_faults.fault[i])) {
            s_faults.fault[i] = sample;
        }
        s_faults.fault[i].failures = failures;
        stored = true;
    }
    if (!stored) {
        if (free_slot < NRF_SENSOR_BUS_FAULT_SLOTS) {
            s_faults.used[free_slot] = true;
            s_faults.fault[free_slot] = sample;
        } else {
            ++s_faults.dropped;
        }
    }
    k_spin_unlock(&s_faults.lock, key);
    return sample;
}

/* An answer from the address ends its failure history: only failures since the last success are worth reporting. */
static void nrf_sensor_bus_zephyr_clear_fault(uint8_t address)
{
    k_spinlock_key_t key = k_spin_lock(&s_faults.lock);
    for (size_t i = 0U; i < NRF_SENSOR_BUS_FAULT_SLOTS; ++i) {
        if (s_faults.used[i] && s_faults.fault[i].address == address) {
            s_faults.used[i] = false;
        }
    }
    k_spin_unlock(&s_faults.lock, key);
}

bool nrf_sensor_bus_zephyr_take_fault(uint8_t first_address, uint8_t last_address, nrf_sensor_bus_fault_t *out)
{
    if (out == NULL || first_address > last_address) {
        return false;
    }
    bool found = false;
    k_spinlock_key_t key = k_spin_lock(&s_faults.lock);
    for (size_t i = 0U; i < NRF_SENSOR_BUS_FAULT_SLOTS; ++i) {
        if (!s_faults.used[i] || s_faults.fault[i].address < first_address ||
            s_faults.fault[i].address > last_address) {
            continue;
        }
        if (!found || nrf_sensor_bus_fault_outranks(&s_faults.fault[i], out)) {
            *out = s_faults.fault[i];
            found = true;
        }
        s_faults.used[i] = false;
    }
    k_spin_unlock(&s_faults.lock, key);
    return found;
}

bool nrf_sensor_bus_zephyr_take_fault_word(uint8_t first_address, uint8_t last_address, uint32_t *packed)
{
    nrf_sensor_bus_fault_t fault;
    if (packed == NULL || !nrf_sensor_bus_zephyr_take_fault(first_address, last_address, &fault)) {
        return false;
    }
    *packed = nrf_sensor_bus_fault_pack(&fault);
    return true;
}

uint32_t nrf_sensor_bus_zephyr_dropped_faults(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_faults.lock);
    const uint32_t dropped = s_faults.dropped;
    k_spin_unlock(&s_faults.lock, key);
    return dropped;
}

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
static const char *nrf_sensor_bus_zephyr_cause_name(nrf_sensor_bus_result_t cause)
{
    switch (cause) {
    case NRF_SENSOR_BUS_ERR_TIMEOUT: return "timeout";
    case NRF_SENSOR_BUS_ERR_IO: return "io";
    default: return "other";
    }
}
#endif

static int nrf_sensor_bus_zephyr_transfer(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    nrf_sensor_bus_zephyr_context_t *zephyr_context =
        (nrf_sensor_bus_zephyr_context_t *)context;
    if (zephyr_context == NULL || zephyr_context->device == NULL) {
        return -ENODEV;
    }
#if SIZE_MAX > UINT32_MAX
    if (write_len > UINT32_MAX || read_len > UINT32_MAX) {
        return -EINVAL;
    }
#endif

    int result;
    if (write_len > 0U && read_len > 0U) {
        result = i2c_write_read(
            zephyr_context->device,
            address,
            write_data,
            write_len,
            read_data,
            read_len);
    } else if (write_len > 0U) {
        result = i2c_write(
            zephyr_context->device,
            write_data,
            (uint32_t)write_len,
            address);
    } else if (read_len > 0U) {
        result = i2c_read(
            zephyr_context->device,
            read_data,
            (uint32_t)read_len,
            address);
    } else {
        return -EINVAL; /* nothing was sent: not a bus fault */
    }

    if (result != 0) {
        const nrf_sensor_bus_fault_t fault =
            nrf_sensor_bus_zephyr_note_fault(zephyr_context->device, address, result);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        printk("sensor_i2c: bus=i2c30 addr=0x%02x reg=%d write=%u read=%u ret=%d cause=%s sda=%d scl=%d\n",
               (unsigned)address,
               write_data != NULL && write_len > 0U ? (int)write_data[0] : -1,
               (unsigned)write_len, (unsigned)read_len, result,
               nrf_sensor_bus_zephyr_cause_name(fault.cause),
               fault.lines_valid ? (fault.sda_high ? 1 : 0) : -1,
               fault.lines_valid ? (fault.scl_high ? 1 : 0) : -1);
#else
        (void)fault;
#endif
    } else {
        nrf_sensor_bus_zephyr_clear_fault(address);
    }
    return result;
}

#define NRF_SENSOR_BUS_SCAN_FIRST_ADDRESS 0x08U
#define NRF_SENSOR_BUS_SCAN_LAST_ADDRESS 0x77U

void nrf_sensor_bus_zephyr_diagnose_bmi_probe(uint8_t address)
{
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    if (address != 0x68U && address != 0x69U) return;
    const struct device *device = s_private_context.device;
    const int resume = pm_device_runtime_get(device);
    if (resume != 0) {
        printk("sensor_probe_slow: bus=i2c30 addr=0x%02x resume=%d\n", (unsigned)address, resume);
        return;
    }
    const int configure = i2c_configure(device, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    const uint8_t reg = 0x00U;
    uint8_t id = 0U;
    const int result = configure == 0 ? i2c_write_read(device, address, &reg, 1U, &id, 1U) : configure;
    /* i2c30's fixed board profile is 400 kHz; do not leave ENV on the diagnostic rate. */
    const int restore = i2c_configure(device, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
    (void)pm_device_runtime_put(device);
    printk("sensor_probe_slow: bus=i2c30 addr=0x%02x hz=100000 configure=%d ret=%d chip_id=%d restore=%d\n",
           (unsigned)address, configure, result, result == 0 ? (int)id : -1, restore);
#else
    (void)address;
#endif
}

int nrf_sensor_bus_zephyr_scan_expected(void)
{
    static const uint8_t addresses[] = {0x68U, 0x69U, 0x76U, 0x77U};
    int found = 0;
    if (!device_is_ready(s_private_context.device)) return -ENODEV;
    for (size_t i = 0U; i < sizeof(addresses); ++i) {
        const uint8_t reg = i < 2U ? 0x00U : 0xD0U;
        uint8_t id = 0U;
        const int result = i2c_write_read(s_private_context.device, addresses[i], &reg, 1U, &id, 1U);
        if (result == 0) ++found;
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        printk("sensor_scan: expected addr=0x%02x id=%d\n", (unsigned)addresses[i],
               result == 0 ? (int)id : -1);
#endif
    }
    return found;
}

int nrf_sensor_bus_zephyr_scan_address(uint8_t address)
{
    static const uint8_t id_registers[] = {0x00U, 0x75U, 0xD0U};
    uint8_t byte = 0U;
    if (address < NRF_SENSOR_BUS_SCAN_FIRST_ADDRESS || address > NRF_SENSOR_BUS_SCAN_LAST_ADDRESS) return -EINVAL;
    if (!device_is_ready(s_private_context.device)) return -ENODEV;
    if (i2c_read(s_private_context.device, &byte, 1U, address) != 0) return 0;
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    int ids[sizeof(id_registers)];
    for (size_t i = 0U; i < sizeof(id_registers); ++i) {
        uint8_t value = 0U;
        ids[i] = i2c_write_read(s_private_context.device, address, &id_registers[i], 1U, &value, 1U) == 0
            ? (int)value : -1;
    }
    printk("sensor_scan: bus=i2c30 addr=0x%02x ack reg00=%d reg75=%d regD0=%d (-1 = no answer)\n",
           (unsigned)address, ids[0], ids[1], ids[2]);
#else
    (void)id_registers;
#endif
    return 1;
}

int nrf_sensor_bus_zephyr_scan_private_bitmap(uint32_t bitmap[4])
{
    int found = 0;
    if (bitmap != NULL) memset(bitmap, 0, 4U * sizeof(*bitmap));
    for (uint16_t address = NRF_SENSOR_BUS_SCAN_FIRST_ADDRESS;
         address <= NRF_SENSOR_BUS_SCAN_LAST_ADDRESS; ++address) {
        const int result = nrf_sensor_bus_zephyr_scan_address((uint8_t)address);
        if (result < 0) return result;
        if (result == 0) continue;
        ++found;
        if (bitmap != NULL) bitmap[address / 32U] |= UINT32_C(1) << (address % 32U);
    }
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    printk("sensor_scan: bus=i2c30 devices=%d\n", found);
#endif
    return found;
}

int nrf_sensor_bus_zephyr_scan_private(void)
{
    return nrf_sensor_bus_zephyr_scan_private_bitmap(NULL);
}

bool nrf_sensor_bus_zephyr_init_private(nrf_sensor_bus_t *out_bus)
{
    if (out_bus == NULL) {
        return false;
    }

    *out_bus = (nrf_sensor_bus_t){0};
    if (!device_is_ready(s_private_context.device)) {
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        printk("sensor_i2c: bus=i2c30 ready=0\n");
#endif
        return false;
    }

    out_bus->transfer = nrf_sensor_bus_zephyr_transfer;
    out_bus->context = &s_private_context;
    return true;
}
