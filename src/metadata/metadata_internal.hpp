#pragma once

#include "runtime/runtime_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace il2cpp_native_bridge::detail {

Result<void> validate_class_handle(const ClassHandle& klass);
Result<void> validate_method_handle(const MethodHandle& method);
Result<void> validate_field_handle(const FieldHandle& field);
Result<std::string> managed_name(const Api& api, const Il2CppType* type);

inline std::string metadata_cache_key(std::string_view kind,
                                      std::uint64_t generation,
                                      const void* owner) {
    return std::string(kind) + ':' + std::to_string(generation) + ':' +
           std::to_string(reinterpret_cast<std::uintptr_t>(owner));
}

inline void append_metadata_cache_part(std::string& key, std::string_view value) {
    key.push_back('|');
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
}

} /// namespace il2cpp_native_bridge::detail
