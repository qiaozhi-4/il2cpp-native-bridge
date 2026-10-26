#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

namespace il2cpp_native_bridge::detail {

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
    const Il2CppImage* (*get_corlib)(){};
    void (*free_memory)(void*){};
};

struct ResolvedExports {
    Api api;
    std::vector<std::string> missing_required;
    std::vector<std::string> missing_optional;

    [[nodiscard]] bool has_required_exports() const noexcept { return missing_required.empty(); }
};

using SymbolLookup = void* (*)(void* module, const char* symbol_name);
using SymbolLookupReady = bool (*)(void* module);

ResolvedExports resolve_exports(void* module,
                                SymbolLookup lookup,
                                const std::unordered_map<std::string, void*>& missing_symbol_overrides = {});

} /// namespace il2cpp_native_bridge::detail
