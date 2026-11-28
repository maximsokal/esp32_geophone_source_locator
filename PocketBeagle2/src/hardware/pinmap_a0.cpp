#include "pb2/hardware/pinmap_a0.hpp"

#include <fstream>
#include <set>
#include <string>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace pb2::hardware {

namespace {

std::string read_trimmed_file(const char* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

} // namespace

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

    /* 2. Fallback to Read After Conversion (Figure 2) for lower rates (incl. PRU loop/ring overhead) */
    constexpr double kPruReadAfterOverheadUs = 1.5;
    const double after_conv_total_us = kT1MaxUs + kTConvMaxUs + res.read_time_us + kTQuietUs + kPruReadAfterOverheadUs;
    if (after_conv_total_us <= res.period_us) {
        res.feasible = true;
        res.reason = "Feasible via Read After Conversion (Figure 2)";
        return res;
    }

    res.feasible = false;
    res.reason = "Exceeds both Read During Conversion (t6) and Read After Conversion (t_CONV_max + read) budgets";
    return res;
}

ClockValidationResult validate_pru_iep_clocks(
    std::uint32_t pru_clock_hz,
    std::uint32_t iep_clock_hz
) {
    ClockValidationResult res{};
    res.pru_clock_hz = pru_clock_hz;
    res.iep_clock_hz = iep_clock_hz;
    if (pru_clock_hz == 0U || iep_clock_hz == 0U) {
        res.reason = "Unknown PRU or IEP clock frequency (0 Hz); refusing hardware ADC acquisition";
        return res;
    }
    if (pru_clock_hz != 200000000U) {
        res.reason = "Unsupported PRU clock frequency (expected 200 MHz for deterministic AD7606 SCLK)";
        return res;
    }
    if (iep_clock_hz < 100000000U || iep_clock_hz > 500000000U) {
        res.reason = "IEP clock frequency out of supported AM62x range";
        return res;
    }
    res.valid = true;
    res.reason = "PRU (200 MHz) and IEP clocks validated";
    return res;
}

bool verify_a1_pinmap_conflict_free() {
    const std::set<std::string> uart2_pins = {"P2.02", "P2.04"};
    const std::set<std::string> conflict_pins = {"P2.20"}; /* aliases PRU1_GPI4 */
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

bool verify_a0_pinmap_conflict_free() {
    return verify_a1_pinmap_conflict_free();
}

BoardRevision detect_board_revision(std::string* out_diagnostic) {
    const std::string dt_compat = read_trimmed_file("/proc/device-tree/compatible");
    const std::string soc_id = read_trimmed_file("/sys/devices/soc0/soc_id");
    const std::string machine = read_trimmed_file("/sys/devices/soc0/machine");
#if defined(__linux__)
    const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
#else
    const long cpus = -1;
#endif

    const bool is_pb2_dt = (dt_compat.find("beagle,am62-pocketbeagle2") != std::string::npos);
    const bool is_am625 = (soc_id.find("AM625") != std::string::npos ||
                           machine.find("AM625") != std::string::npos ||
                           dt_compat.find("ti,am625") != std::string::npos);

    BoardRevision rev = BoardRevision::Unknown;
    if (is_pb2_dt && is_am625 && cpus == 4L) {
        rev = BoardRevision::RevA1;
    } else if (is_pb2_dt && cpus == 2L) {
        rev = BoardRevision::RevA0Legacy;
    }

    if (out_diagnostic) {
        *out_diagnostic = "dt_compat=" + (dt_compat.empty() ? std::string("unavailable") : dt_compat) +
                          " soc_id=" + (soc_id.empty() ? std::string("unavailable") : soc_id) +
                          " cpus=" + std::to_string(cpus) +
                          " detected=" + board_revision_name(rev);
    }
    return rev;
}

const char* board_revision_name(BoardRevision rev) noexcept {
    switch (rev) {
        case BoardRevision::RevA1:       return "PB2VA1_A1_AM6254";
        case BoardRevision::RevA0Legacy: return "PB2_A0_LEGACY_AM6232";
        default:                         return "UNKNOWN";
    }
}

} // namespace pb2::hardware
