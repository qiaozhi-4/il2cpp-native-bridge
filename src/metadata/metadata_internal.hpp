#pragma once

#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/runtime_internal.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace INB::detail {

/// IL2CPP 方法标志位中的 static 位。
inline constexpr std::uint32_t kMethodAttributeStatic = 0x0010;
/// IL2CPP 字段标志位中的 static 位。
inline constexpr std::uint32_t kFieldAttributeStatic = 0x0010;

struct InvokeOutcome {
    Il2CppObject* value{};
};

Result<void> validate_class_handle(const ClassHandle& klass);
Result<void> validate_method_handle(const MethodHandle& method);
Result<void> validate_field_handle(const FieldHandle& field);
Result<std::string> managed_name(const Api& api, const Il2CppType* type);

/// 判断镜像名与请求的程序集名是否指同一程序集，容忍 ".dll" 后缀差异。
bool assembly_name_matches(std::string_view image_name, std::string_view requested_name);

Result<ClassHandle> resolve_class(const std::shared_ptr<RuntimeContext>& context,
                                  std::string_view full_name,
                                  const std::optional<std::string>& assembly_name);
Result<std::vector<ClassHandle>> resolve_classes(const std::shared_ptr<RuntimeContext>& context,
                                                 std::string_view name_filter);
Result<ClassHandle> resolve_generic_class(const std::shared_ptr<RuntimeContext>& context,
                                          std::string_view generic_type_name,
                                          std::span<const std::string_view> type_arguments,
                                          const std::optional<std::string>& assembly_name);
Result<ClassHandle> resolve_object_class(const std::shared_ptr<RuntimeContext>& context, Il2CppObject* instance);
Result<std::vector<MethodMetadata>> resolve_methods(const ClassHandle& klass);
Result<std::vector<FieldHandle>> resolve_fields(const ClassHandle& klass);
Result<Il2CppObject*> allocate_object(const ClassHandle& klass);

Result<MethodHandle> resolve_method(const ClassHandle& klass,
                                    std::string_view name,
                                    std::string_view return_type,
                                    const std::vector<std::string>& parameter_types);
Result<MethodHandle> resolve_method_by_arity(const ClassHandle& klass,
                                             std::string_view name,
                                             std::size_t parameter_count);
Result<FieldHandle> resolve_field(const ClassHandle& klass,
                                  std::string_view name,
                                  std::string_view value_type);

Result<void> read_field_raw(const FieldHandle& field, Il2CppObject* instance, void* destination, std::size_t size);
Result<void> write_field_raw(const FieldHandle& field, Il2CppObject* instance, void* source, std::size_t size);
Result<void> write_boxed_field(const FieldHandle& field, Il2CppObject* boxed_value, Il2CppObject* instance);

Result<InvokeOutcome> invoke_raw(const MethodHandle& method, Il2CppObject* instance, std::span<void*> arguments);

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

} /// namespace INB::detail
