#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace il2cpp_native_bridge {

enum class RuntimeState {
    waiting_for_module,
    initializing,
    ready,
    unsupported,
};

struct RuntimeSnapshot {
    RuntimeState state{RuntimeState::waiting_for_module};
    std::uint64_t generation{0};
    std::vector<std::string> capabilities;
    std::vector<std::string> missing_apis;
    std::vector<std::string> missing_optional_apis;
    std::string last_error;
};

} /// namespace il2cpp_native_bridge
