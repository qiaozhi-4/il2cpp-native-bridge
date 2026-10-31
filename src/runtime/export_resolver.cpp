#include "runtime/il2cpp_api.hpp"

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

    /// 绑定与缺失记录来自同一份导出清单，避免字段名与符号名各写一遍后漂移。
#define IL2CPP_BIND_REQUIRED(member, symbol)                                                              \
    result.api.member = reinterpret_cast<decltype(result.api.member)>(                                     \
            resolve_symbol(module, lookup, missing_symbol_overrides, symbol));                             \
    if (result.api.member == nullptr) result.missing_required.emplace_back(symbol);
#define IL2CPP_BIND_OPTIONAL(member, symbol, group)                                                       \
    result.api.member = reinterpret_cast<decltype(result.api.member)>(                                     \
            resolve_symbol(module, lookup, missing_symbol_overrides, symbol));                             \
    if (result.api.member == nullptr) {                                                                    \
        result.missing_optional.emplace_back(symbol);                                                      \
        result.missing_by_group[group].emplace_back(symbol);                                               \
    }

    IL2CPP_NATIVE_BRIDGE_EXPORT_LIST(IL2CPP_BIND_REQUIRED, IL2CPP_BIND_OPTIONAL)

#undef IL2CPP_BIND_REQUIRED
#undef IL2CPP_BIND_OPTIONAL
    return result;
}

} /// namespace il2cpp_native_bridge::detail
