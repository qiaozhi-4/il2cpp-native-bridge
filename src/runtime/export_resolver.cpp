#include "il2cpp_native_bridge/detail/il2cpp_api.hpp"

#include <array>

namespace il2cpp_native_bridge::detail {
namespace {

void* resolve_symbol(void* module,
                     SymbolLookup lookup,
                     const std::unordered_map<std::string, void*>& overrides,
                     const char* name) {
    if (void* address = lookup(module, name); address != nullptr) return address;
    const auto override = overrides.find(name);
    return override == overrides.end() ? nullptr : override->second;
}

} /// namespace

ResolvedExports resolve_exports(void* module,
                                SymbolLookup lookup,
                                const std::unordered_map<std::string, void*>& missing_symbol_overrides) {
    ResolvedExports result;
    if (lookup == nullptr) {
        result.missing_required.emplace_back("symbol resolver");
        return result;
    }

#define IL2CPP_REQUIRED(member, symbol) \
    result.api.member = reinterpret_cast<decltype(result.api.member)>(resolve_symbol(module, lookup, missing_symbol_overrides, symbol)); \
    if (result.api.member == nullptr) result.missing_required.emplace_back(symbol)
#define IL2CPP_OPTIONAL(member, symbol) \
    result.api.member = reinterpret_cast<decltype(result.api.member)>(resolve_symbol(module, lookup, missing_symbol_overrides, symbol)); \
    if (result.api.member == nullptr) result.missing_optional.emplace_back(symbol)

    IL2CPP_REQUIRED(domain_get, "il2cpp_domain_get");
    IL2CPP_REQUIRED(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    IL2CPP_REQUIRED(assembly_get_image, "il2cpp_assembly_get_image");
    IL2CPP_REQUIRED(image_get_name, "il2cpp_image_get_name");
    IL2CPP_REQUIRED(class_from_name, "il2cpp_class_from_name");
    IL2CPP_REQUIRED(class_get_name, "il2cpp_class_get_name");
    IL2CPP_REQUIRED(class_get_namespace, "il2cpp_class_get_namespace");
    IL2CPP_REQUIRED(class_get_methods, "il2cpp_class_get_methods");
    IL2CPP_REQUIRED(method_get_name, "il2cpp_method_get_name");
    IL2CPP_REQUIRED(method_get_param_count, "il2cpp_method_get_param_count");
    IL2CPP_REQUIRED(method_get_param, "il2cpp_method_get_param");
    IL2CPP_REQUIRED(method_get_return_type, "il2cpp_method_get_return_type");
    IL2CPP_REQUIRED(type_get_name, "il2cpp_type_get_name");
    IL2CPP_REQUIRED(method_get_flags, "il2cpp_method_get_flags");
    IL2CPP_REQUIRED(class_get_fields, "il2cpp_class_get_fields");
    IL2CPP_REQUIRED(field_get_name, "il2cpp_field_get_name");
    IL2CPP_REQUIRED(field_get_type, "il2cpp_field_get_type");
    IL2CPP_REQUIRED(field_get_flags, "il2cpp_field_get_flags");
    IL2CPP_REQUIRED(field_static_get_value, "il2cpp_field_static_get_value");
    IL2CPP_REQUIRED(field_static_set_value, "il2cpp_field_static_set_value");
    IL2CPP_REQUIRED(field_get_value, "il2cpp_field_get_value");
    IL2CPP_REQUIRED(field_set_value, "il2cpp_field_set_value");
    IL2CPP_REQUIRED(runtime_invoke, "il2cpp_runtime_invoke");
    IL2CPP_REQUIRED(object_unbox, "il2cpp_object_unbox");
    IL2CPP_REQUIRED(thread_current, "il2cpp_thread_current");
    IL2CPP_REQUIRED(thread_attach, "il2cpp_thread_attach");
    IL2CPP_REQUIRED(thread_detach, "il2cpp_thread_detach");
    IL2CPP_REQUIRED(free_memory, "il2cpp_free");

    IL2CPP_OPTIONAL(object_get_class, "il2cpp_object_get_class");
    IL2CPP_OPTIONAL(object_new, "il2cpp_object_new");
    IL2CPP_OPTIONAL(class_get_type, "il2cpp_class_get_type");
    IL2CPP_OPTIONAL(type_get_object, "il2cpp_type_get_object");
    IL2CPP_OPTIONAL(array_new, "il2cpp_array_new");
    IL2CPP_OPTIONAL(class_from_system_type, "il2cpp_class_from_system_type");
    IL2CPP_OPTIONAL(get_corlib, "il2cpp_get_corlib");
    IL2CPP_OPTIONAL(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    IL2CPP_OPTIONAL(method_get_from_reflection, "il2cpp_method_get_from_reflection");
    IL2CPP_OPTIONAL(runtime_class_init, "il2cpp_runtime_class_init");
    IL2CPP_OPTIONAL(image_get_class_count, "il2cpp_image_get_class_count");
    IL2CPP_OPTIONAL(image_get_class, "il2cpp_image_get_class");

#undef IL2CPP_REQUIRED
#undef IL2CPP_OPTIONAL
    return result;
}

} /// namespace il2cpp_native_bridge::detail
