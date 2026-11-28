#include "pb2/hardware/pinmap_a0.hpp"

#include <set>
#include <string>

namespace pb2::hardware {

Ad7606TimingCheck evaluate_ad7606_timing(
    pb2::acquisition::AdcWiringProfile profile,
    std::uint32_t sample_rate_hz,
    double sclk_mhz
) {
    Ad7606TimingCheck res{};
    if (sample_rate_hz == 0U || sclk_mhz <= 0.0) {
        res.reason = "Invalid sample rate or SCLK frequency";
        return res;
    }

    /* AD7606 Rev. G Table 3: at VDRIVE = 3.3 V (2.7 V..3.3 V range), max SCLK is 12.5 MHz. */
    if (sclk_mhz > 12.5) {
        res.reason = "SCLK exceeds 12.5 MHz limit for VDRIVE = 3.3 V (Table 3)";
        return res;
    }

    res.sclk_cycles = (profile == pb2::acquisition::AdcWiringProfile::SerialDualDout) ? 32U : 64U;
    res.period_us = 1000000.0 / static_cast<double>(sample_rate_hz);
    res.read_time_us = static_cast<double>(res.sclk_cycles) / sclk_mhz;

    /* AD7606 Rev. G Table 3 (OS off): t_CONV 3.45..4.2 us, t1 <= 45 ns, t6 >= 25 ns, t_QUIET >= 50 ns */
    constexpr double kTConvMinUs = 3.45;
    constexpr double kTConvMaxUs = 4.20;
    constexpr double kT1MaxUs = 0.045;
    constexpr double kT6MinNs = 25.0;
    constexpr double kTQuietUs = 0.05;

    const double available_busy_high_us = kTConvMinUs - kT1MaxUs;
    res.margin_before_busy_fall_ns = (available_busy_high_us - res.read_time_us) * 1000.0;

    /* 1. Try Read During Conversion (Figure 3) */
    if (res.read_time_us < res.period_us && res.margin_before_busy_fall_ns >= kT6MinNs) {
        res.feasible = true;
        res.reason = "Feasible via Read During Conversion (Figure 3) within 3.3 V Table 3 + t6 guard";
        return res;
    }

    /* 2. Fallback to Read After Conversion (Figure 2) for lower rates */
    const double after_conv_total_us = kT1MaxUs + kTConvMaxUs + res.read_time_us + kTQuietUs;
    if (after_conv_total_us <= res.period_us) {
        res.feasible = true;
        res.reason = "Feasible via Read After Conversion (Figure 2)";
        return res;
    }

    res.feasible = false;
    res.reason = "Exceeds both Read During Conversion (t6) and Read After Conversion (t_CONV_max + read) budgets";
    return res;
}

bool verify_a0_pinmap_conflict_free() {
    const std::set<std::string> uart2_pins = {"P2.02", "P2.04"};
    const std::set<std::string> conflict_pins = {"P2.20"}; /* aliases PR0_PRU1_GPI4 */
    const std::array<std::string, 8> pru1_adc_pins = {
        "P2.06", "P2.08", "P2.30", "P2.32", "P2.10", "P2.24", "P2.33", "P2.28"
    };

    std::set<std::string> seen;
    for (const auto& p : pru1_adc_pins) {
        if (!seen.insert(p).second) return false;
        if (uart2_pins.count(p) || conflict_pins.count(p)) return false;
    }
    return true;
}

} // namespace pb2::hardware
