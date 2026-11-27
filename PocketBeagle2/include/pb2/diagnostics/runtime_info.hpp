#pragma once

#include <string>

namespace pb2::diagnostics {

struct RuntimeInfo {
    std::string app_version;
    std::string arch;
    std::string os_name;
    std::string board_target;
};

RuntimeInfo collect_runtime_info();

} // namespace pb2::diagnostics
