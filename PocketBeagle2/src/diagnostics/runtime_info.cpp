#include "pb2/diagnostics/runtime_info.hpp"

namespace pb2::diagnostics {

RuntimeInfo collect_runtime_info() {
    RuntimeInfo info{};
    info.app_version = "0.1.0";
    info.board_target = "PocketBeagle 2 Rev A0 (AM6232)";

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
