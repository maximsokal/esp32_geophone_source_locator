#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>

/*
 * This is a Zephyr driver-level fake, not a copy of the production sensor
 * bridge. The real nrf_sensor_bus_zephyr.c obtains i2c30 through DT and calls
 * the public Zephyr I2C API; this device supplies deterministic register
 * responses for that exact path.
 */
typedef struct {
	uint8_t registers[256];
	uint8_t current_register;
} fake_i2c_state_t;

static fake_i2c_state_t s_state = {
	.registers = {
		[0x00U] = 0xD1U, /* BMI160 chip id */
		[0x03U] = 0x14U, /* BMI160 accel+gyro normal */
		[0xD0U] = 0x60U, /* BME280 chip id at 0x76 */
		[0x8EU] = 0x5DU, /* BME280 dig_p1 = 0x8E5D, non-zero */
		[0x8FU] = 0x8EU,
	},
};

static int fake_i2c_transfer(
	const struct device *dev,
	struct i2c_msg *msgs,
	uint8_t num_msgs,
	uint16_t address)
{
	fake_i2c_state_t *state = (fake_i2c_state_t *)dev->data;
	uint8_t register_address;

	(void)address;
	if (state == NULL || msgs == NULL || num_msgs == 0U || num_msgs > 2U) {
		return -EINVAL;
	}

	if ((msgs[0].flags & I2C_MSG_READ) == 0U) {
		if (msgs[0].buf == NULL || msgs[0].len == 0U) {
			return -EINVAL;
		}
		state->current_register = msgs[0].buf[0];
		if (msgs[0].len > 1U) {
			for (uint32_t index = 1U; index < msgs[0].len; ++index) {
				state->registers[(uint8_t)(state->current_register + index - 1U)] =
					msgs[0].buf[index];
			}
		}
	}

	if (num_msgs == 1U && (msgs[0].flags & I2C_MSG_READ) != 0U) {
		register_address = state->current_register;
		if (msgs[0].buf == NULL) {
			return -EINVAL;
		}
		for (uint32_t index = 0U; index < msgs[0].len; ++index) {
			msgs[0].buf[index] = state->registers[(uint8_t)(register_address + index)];
		}
	}

	if (num_msgs == 2U && (msgs[1].flags & I2C_MSG_READ) != 0U) {
		register_address = state->current_register;
		if (msgs[1].buf == NULL) {
			return -EINVAL;
		}
		for (uint32_t index = 0U; index < msgs[1].len; ++index) {
			msgs[1].buf[index] = state->registers[(uint8_t)(register_address + index)];
		}
	}

	return 0;
}

static int fake_i2c_init(const struct device *dev)
{
	(void)dev;
	return 0;
}

static const struct i2c_driver_api s_fake_i2c_api = {
	.transfer = fake_i2c_transfer,
};

I2C_DEVICE_DT_DEFINE(
	DT_NODELABEL(i2c30),
	fake_i2c_init,
	NULL,
	&s_state,
	NULL,
	POST_KERNEL,
	CONFIG_I2C_INIT_PRIORITY,
	&s_fake_i2c_api);
