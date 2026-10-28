#pragma once

#include <stdbool.h>

/*
 * Test control of the i2c30 pin model in nrf_line_sampling_stub.c. The model mirrors the real controller: after
 * pm_device_runtime_put() the sleep pinctrl state disconnects the input buffers, so the pins read low until the
 * device is resumed again.
 */
void test_line_stub_reset(void);
void test_line_stub_set_levels(bool sda_high, bool scl_high);
void test_line_stub_fail_resume(bool fail);
unsigned test_line_stub_resume_count(void);
unsigned test_line_stub_suspend_count(void);
unsigned test_line_stub_reads_while_suspended(void);
