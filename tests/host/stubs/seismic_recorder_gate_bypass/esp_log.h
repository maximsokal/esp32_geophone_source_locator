#pragma once

void seismic_recorder_gate_bypass_test_log(
    const char *tag,
    const char *format,
    ...
);

#define ESP_LOGI(...) seismic_recorder_gate_bypass_test_log(__VA_ARGS__)
#define ESP_LOGW(...) seismic_recorder_gate_bypass_test_log(__VA_ARGS__)
#define ESP_LOGE(...) seismic_recorder_gate_bypass_test_log(__VA_ARGS__)
