#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppType;
struct Il2CppMethodInfo;
struct Il2CppFieldInfo;
struct Il2CppObject;
struct Il2CppArray;
struct Il2CppString;
struct Il2CppThread;

namespace INB::detail {

struct Api {
    Il2CppDomain* (*domain_get)(){};
    const Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, std::size_t*){};
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*){};
    const char* (*image_get_name)(const Il2CppImage*){};
    std::size_t (*image_get_class_count)(const Il2CppImage*){};
    Il2CppClass* (*image_get_class)(const Il2CppImage*, std::size_t){};
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*){};
    const char* (*class_get_name)(Il2CppClass*){};
    const char* (*class_get_namespace)(Il2CppClass*){};
    const Il2CppMethodInfo* (*class_get_methods)(Il2CppClass*, void**){};
    const char* (*method_get_name)(const Il2CppMethodInfo*){};
    std::uint32_t (*method_get_param_count)(const Il2CppMethodInfo*){};
    const Il2CppType* (*method_get_param)(const Il2CppMethodInfo*, std::uint32_t){};
    const Il2CppType* (*method_get_return_type)(const Il2CppMethodInfo*){};
    char* (*type_get_name)(const Il2CppType*){};
    std::uint32_t (*method_get_flags)(const Il2CppMethodInfo*, std::uint32_t*){};
    const Il2CppFieldInfo* (*class_get_fields)(Il2CppClass*, void**){};
    const char* (*field_get_name)(const Il2CppFieldInfo*){};
    const Il2CppType* (*field_get_type)(const Il2CppFieldInfo*){};
    std::uint32_t (*field_get_flags)(const Il2CppFieldInfo*){};
    void (*field_static_get_value)(const Il2CppFieldInfo*, void*){};
    void (*field_static_set_value)(const Il2CppFieldInfo*, void*){};
    void (*field_get_value)(Il2CppObject*, const Il2CppFieldInfo*, void*){};
    void (*field_set_value)(Il2CppObject*, const Il2CppFieldInfo*, void*){};
    Il2CppObject* (*runtime_invoke)(const Il2CppMethodInfo*, void*, void**, Il2CppObject**){};
    void* (*object_unbox)(Il2CppObject*){};
    Il2CppThread* (*thread_current)(){};
    Il2CppThread* (*thread_attach)(Il2CppDomain*){};
    void (*thread_detach)(Il2CppThread*){};

    Il2CppClass* (*object_get_class)(Il2CppObject*){};
    Il2CppObject* (*object_new)(Il2CppClass*){};
    const Il2CppType* (*class_get_type)(Il2CppClass*){};
    Il2CppObject* (*type_get_object)(const Il2CppType*){};
    Il2CppArray* (*array_new)(Il2CppClass*, std::size_t){};
    Il2CppClass* (*class_from_system_type)(Il2CppObject*){};
    Il2CppClass* (*class_get_parent)(Il2CppClass*){};
    const Il2CppImage* (*get_corlib)(){};
    std::size_t (*array_length)(Il2CppArray*){};
    Il2CppString* (*string_new_utf16)(const std::uint16_t*, std::uint32_t){};
    void (*free_memory)(void*){};

    /// 按需类型识别、赋值兼容与字符串读取；缺失只影响依赖它的操作，不阻断就绪判断。
    int (*type_get_type)(const Il2CppType*){};
    bool (*type_is_byref)(const Il2CppType*){};
    Il2CppClass* (*class_from_type)(const Il2CppType*){};
    bool (*class_is_valuetype)(Il2CppClass*){};
    bool (*class_is_assignable_from)(Il2CppClass*, Il2CppClass*){};
    Il2CppClass* (*method_get_class)(const Il2CppMethodInfo*){};
    bool (*method_is_generic)(const Il2CppMethodInfo*){};
    bool (*method_is_inflated)(const Il2CppMethodInfo*){};
    bool (*class_is_generic)(Il2CppClass*){};
    bool (*class_is_inflated)(Il2CppClass*){};
    std::int32_t (*string_length)(Il2CppString*){};
    const std::uint16_t* (*string_chars)(Il2CppString*){};
};

/// 能力分组名：诊断与按需失败共用同一组标识。
namespace capability_group {
inline constexpr const char* kReadiness = "readiness";
inline constexpr const char* kEnumeration = "enumeration";
inline constexpr const char* kAllocation = "allocation";
inline constexpr const char* kType = "type";
inline constexpr const char* kInstance = "instance";
inline constexpr const char* kString = "string";
inline constexpr const char* kArray = "array";
} /// namespace capability_group

/// 导出表清单：字段、符号名，以及可选导出的能力分组。绑定、诊断与测试桩共用这一份定义。
#define IL2CPP_NATIVE_BRIDGE_EXPORT_LIST(X_REQUIRED, X_OPTIONAL)                              \
    X_REQUIRED(domain_get, "il2cpp_domain_get")                                                \
    X_REQUIRED(domain_get_assemblies, "il2cpp_domain_get_assemblies")                          \
    X_REQUIRED(assembly_get_image, "il2cpp_assembly_get_image")                                \
    X_REQUIRED(image_get_name, "il2cpp_image_get_name")                                        \
    X_REQUIRED(class_from_name, "il2cpp_class_from_name")                                      \
    X_REQUIRED(class_get_name, "il2cpp_class_get_name")                                        \
    X_REQUIRED(class_get_namespace, "il2cpp_class_get_namespace")                              \
    X_REQUIRED(class_get_methods, "il2cpp_class_get_methods")                                  \
    X_REQUIRED(method_get_name, "il2cpp_method_get_name")                                      \
    X_REQUIRED(method_get_param_count, "il2cpp_method_get_param_count")                        \
    X_REQUIRED(method_get_param, "il2cpp_method_get_param")                                    \
    X_REQUIRED(method_get_return_type, "il2cpp_method_get_return_type")                        \
    X_REQUIRED(type_get_name, "il2cpp_type_get_name")                                          \
    X_REQUIRED(method_get_flags, "il2cpp_method_get_flags")                                    \
    X_REQUIRED(class_get_fields, "il2cpp_class_get_fields")                                    \
    X_REQUIRED(field_get_name, "il2cpp_field_get_name")                                        \
    X_REQUIRED(field_get_type, "il2cpp_field_get_type")                                        \
    X_REQUIRED(field_get_flags, "il2cpp_field_get_flags")                                      \
    X_REQUIRED(field_static_get_value, "il2cpp_field_static_get_value")                        \
    X_REQUIRED(field_static_set_value, "il2cpp_field_static_set_value")                        \
    X_REQUIRED(field_get_value, "il2cpp_field_get_value")                                      \
    X_REQUIRED(field_set_value, "il2cpp_field_set_value")                                      \
    X_REQUIRED(runtime_invoke, "il2cpp_runtime_invoke")                                        \
    X_REQUIRED(object_unbox, "il2cpp_object_unbox")                                            \
    X_REQUIRED(thread_current, "il2cpp_thread_current")                                        \
    X_REQUIRED(thread_attach, "il2cpp_thread_attach")                                          \
    X_REQUIRED(thread_detach, "il2cpp_thread_detach")                                          \
    X_REQUIRED(free_memory, "il2cpp_free")                                                     \
    X_OPTIONAL(object_get_class, "il2cpp_object_get_class", capability_group::kInstance)        \
    X_OPTIONAL(object_new, "il2cpp_object_new", capability_group::kAllocation)                  \
    X_OPTIONAL(class_get_type, "il2cpp_class_get_type", capability_group::kAllocation)          \
    X_OPTIONAL(type_get_object, "il2cpp_type_get_object", capability_group::kAllocation)        \
    X_OPTIONAL(array_new, "il2cpp_array_new", capability_group::kAllocation)                    \
    X_OPTIONAL(class_from_system_type, "il2cpp_class_from_system_type", capability_group::kAllocation) \
    X_OPTIONAL(class_get_parent, "il2cpp_class_get_parent", capability_group::kType)                  \
    X_OPTIONAL(get_corlib, "il2cpp_get_corlib", capability_group::kReadiness)                   \
    X_OPTIONAL(array_length, "il2cpp_array_length", capability_group::kArray)                   \
    X_OPTIONAL(string_new_utf16, "il2cpp_string_new_utf16", capability_group::kString)          \
    X_OPTIONAL(image_get_class_count, "il2cpp_image_get_class_count", capability_group::kEnumeration) \
    X_OPTIONAL(image_get_class, "il2cpp_image_get_class", capability_group::kEnumeration)       \
    X_OPTIONAL(type_get_type, "il2cpp_type_get_type", capability_group::kType)                  \
    X_OPTIONAL(type_is_byref, "il2cpp_type_is_byref", capability_group::kType)                  \
    X_OPTIONAL(class_from_type, "il2cpp_class_from_type", capability_group::kType)              \
    X_OPTIONAL(class_is_valuetype, "il2cpp_class_is_valuetype", capability_group::kType)        \
    X_OPTIONAL(class_is_assignable_from, "il2cpp_class_is_assignable_from", capability_group::kType) \
    X_OPTIONAL(method_get_class, "il2cpp_method_get_class", capability_group::kType)            \
    X_OPTIONAL(method_is_generic, "il2cpp_method_is_generic", capability_group::kType)          \
    X_OPTIONAL(method_is_inflated, "il2cpp_method_is_inflated", capability_group::kType)        \
    X_OPTIONAL(class_is_generic, "il2cpp_class_is_generic", capability_group::kType)            \
    X_OPTIONAL(class_is_inflated, "il2cpp_class_is_inflated", capability_group::kType)          \
    X_OPTIONAL(string_length, "il2cpp_string_length", capability_group::kString)                \
    X_OPTIONAL(string_chars, "il2cpp_string_chars", capability_group::kString)

struct ResolvedExports {
    Api api;
    std::vector<std::string> missing_required;
    std::vector<std::string> missing_optional;
    /// 缺失的可选导出按用途分组，供按需能力判断与诊断使用。
    std::unordered_map<std::string, std::vector<std::string>> missing_by_group;

    [[nodiscard]] bool has_required_exports() const noexcept { return missing_required.empty(); }

    /// 该组是否全部可用；未出现在表中的组视为可用。
    [[nodiscard]] bool has_group(std::string_view group) const {
        return missing_by_group.find(std::string(group)) == missing_by_group.end();
    }
};

/// 宿主或平台提供的导出符号查找函数。
using SymbolLookup = void* (*)(void* module, const char* symbol_name);
/// 判断模块是否已完成符号解析器的准备工作。
using SymbolLookupReady = bool (*)(void* module);

/**
 * 按导出清单解析全部 IL2CPP API，并记录缺失项及其能力分组。
 *
 * @param module 要解析的模块句柄。
 * @param lookup 符号查找函数，不能为空。
 * @param missing_symbol_overrides 缺失符号的地址覆盖表。
 * @param log_resolution 为真时按符号逐条打印解析结果；调用方负责保证每次运行只传一次真，
 *                       否则轮询重试会把同一批导出刷进 logcat。
 * @return 解析结果；lookup 为空时所有必需导出记为缺失。
 */
ResolvedExports resolve_exports(void* module,
                                SymbolLookup lookup,
                                const std::unordered_map<std::string, void*>& missing_symbol_overrides = {},
                                bool log_resolution = false);

} /// namespace INB::detail
