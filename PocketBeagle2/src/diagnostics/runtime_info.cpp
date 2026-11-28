#include "pb2/diagnostics/runtime_info.hpp"
#include "pb2/hardware/pinmap_a0.hpp"

namespace pb2::diagnostics {

RuntimeInfo collect_runtime_info() {
    RuntimeInfo info{};
    info.app_version = "0.1.0";
    std::string diag;
    const auto rev = pb2::hardware::detect_board_revision(&diag);
    if (rev == pb2::hardware::BoardRevision::RevA1) {
        info.board_target = "PocketBeagle 2 Rev A1 (PB2VA1 / AM6254A)";
    } else if (rev == pb2::hardware::BoardRevision::RevA0Legacy) {
        info.board_target = "PocketBeagle 2 Rev A0 Legacy (AM6232)";
    } else {
        info.board_target = "PocketBeagle 2 Rev A1 (AM6254A) [host/unverified: " + diag + "]";
    }

#if defined(__aarch64__)
    info.arch = "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    info.arch = "x86_64";
#else
    info.arch = "unknown";
#endif

#if defined(__linux__)
    info.os_name = "Linux";
#else
    info.os_name = "Non-Linux";
#endif

    return info;
}

} // namespace pb2::diagnostics
