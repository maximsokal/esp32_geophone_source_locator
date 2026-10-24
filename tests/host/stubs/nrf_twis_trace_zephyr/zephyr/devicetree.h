#pragma once

extern NRF_TWIS_Type test_upstream_registers;
#define DT_NODELABEL(label) 0
#define DT_REG_ADDR(node) (&test_upstream_registers)
