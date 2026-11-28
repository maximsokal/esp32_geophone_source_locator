#pragma once

#include "pb2/acquisition/mock_source.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace pb2::hardware {

enum class BoardRevision : std::uint8_t {
    Unknown = 0,
    RevA1 = 1,       /* PB2VA1 / TI AM6254A (4x A53, 512 MB LPDDR4) */
    RevA0Legacy = 2, /* Legacy A0 / TI AM6232 (2x A53) */
};

enum class AdcOwnerMode : std::uint8_t {
    SafeDisconnected = 0,
    Esp32Owner = 1,
    BeagleOwner = 2
};

struct Ad7606TimingCheck {
    bool feasible = false;
    std::uint32_t sclk_cycles = 0;
    double period_us = 0.0;
    double read_time_us = 0.0;
    double margin_before_busy_fall_ns = 0.0;
    std::string reason;
};

struct ClockValidationResult {
    bool valid = false;
    std::uint32_t pru_clock_hz = 0;
    std::uint32_t iep_clock_hz = 0;
    std::string reason;
};

/// Validates AD7606 Rev. G Table 3 / Figure 6 / Figure 46 timing at VDRIVE = 3.3 V
/// (f_SCLK max = 12.5 MHz, t_CONV min = 3.45 us, t6 >= 25 ns before BUSY falling).
Ad7606TimingCheck evaluate_ad7606_timing(
    pb2::acquisition::AdcWiringProfile profile,
    std::uint32_t sample_rate_hz,
    double sclk_mhz = 12.5
);

/// Validates PRU1 and IEP0 clock frequencies independently.
/// Rejects unknown (0) or incompatible frequencies for hardware acquisition.
ClockValidationResult validate_pru_iep_clocks(
    std::uint32_t pru_clock_hz,
    std::uint32_t iep_clock_hz
);

/// Returns false if any reserved/conflicting header pin (e.g. P2.20 aliasing R31.4,
/// or P2.02/P2.04 UART2) is assigned to ADC PRU1.
bool verify_a1_pinmap_conflict_free();
bool verify_a0_pinmap_conflict_free(); /* Legacy alias */

/// Reads Linux sysfs/procfs/EEPROM read-only indicators without guessing.
BoardRevision detect_board_revision(std::string* out_diagnostic = nullptr);
const char* board_revision_name(BoardRevision rev) noexcept;

} // namespace pb2::hardware
