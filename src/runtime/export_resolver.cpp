#include "runtime/il2cpp_api.hpp"

#include "logging/internal_log.hpp"

#if defined(__ANDROID__) || defined(__linux__)
#include <dlfcn.h>
#endif

namespace INB::detail {
    namespace {

        void* resolve_symbol(void* module,
                             SymbolLookup lookup,
                             const std::unordered_map<std::string, void*>& overrides,
                             const char* name) {
            if (void* address = lookup(module, name); address != nullptr) return address;
            const auto override = overrides.find(name);
            return override == overrides.end() ? nullptr : override->second;
        }

        /// 导出地址相对所属模块基址的偏移；无法定位时返回 0。跨进程比对同一导出时用得上。
        [[maybe_unused]] std::size_t symbol_offset(void* address) {
        #if defined(__ANDROID__) || defined(__linux__)
            if (address == nullptr) return 0;
            Dl_info info{};
            if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr) return 0;
            return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(address) -
                                            reinterpret_cast<std::uintptr_t>(info.dli_fbase));
        #else
            (void) address;
            return 0;
        #endif
        }

        /// 参考 BNM 的 INIT_IL2CPP_API：每个导出解析出结果就打一行，附带可跨进程比对的模块偏移。
        /// 必需导出缺失始终记录；逐符号明细只在轨迹构建输出。
        void log_export(const char* symbol, void* address, bool required) {
            if (address == nullptr && required) INB_LOGE("export %s: missing", symbol);
            if constexpr (INB_LOG_ENABLED) {
                if (address == nullptr) {
                    INB_LOGD("export %s: missing (optional capability)", symbol);
                } else {
                    INB_LOGD("export %s: %p (+0x%zx)", symbol, address, symbol_offset(address));
                }
            }
        }

    } /// namespace

    ResolvedExports resolve_exports(void* module,
                                    SymbolLookup lookup,
                                    const std::unordered_map<std::string, void*>& missing_symbol_overrides,
                                    bool log_resolution) {
        ResolvedExports result;
        if (lookup == nullptr) {
            result.missing_required.emplace_back("symbol resolver");
            return result;
        }

        /// 绑定与缺失记录来自同一份导出清单，避免字段名与符号名各写一遍后漂移。
    #define IL2CPP_BIND_REQUIRED(member, symbol)                                                              \
        result.api.member = reinterpret_cast<decltype(result.api.member)>(                                     \
                resolve_symbol(module, lookup, missing_symbol_overrides, symbol));                             \
        if (log_resolution) log_export(symbol, reinterpret_cast<void*>(result.api.member), true);              \
        if (result.api.member == nullptr) result.missing_required.emplace_back(symbol);
    #define IL2CPP_BIND_OPTIONAL(member, symbol, group)                                                       \
        result.api.member = reinterpret_cast<decltype(result.api.member)>(                                     \
                resolve_symbol(module, lookup, missing_symbol_overrides, symbol));                             \
        if (log_resolution) log_export(symbol, reinterpret_cast<void*>(result.api.member), false);             \
        if (result.api.member == nullptr) {                                                                    \
            result.missing_optional.emplace_back(symbol);                                                      \
            result.missing_by_group[group].emplace_back(symbol);                                               \
        }

        IL2CPP_NATIVE_BRIDGE_EXPORT_LIST(IL2CPP_BIND_REQUIRED, IL2CPP_BIND_OPTIONAL)

    #undef IL2CPP_BIND_REQUIRED
    #undef IL2CPP_BIND_OPTIONAL

        if (log_resolution) {
            INB_LOGI("exports resolved: %zu required missing, %zu optional unavailable",
                     result.missing_required.size(), result.missing_optional.size());
        }
        return result;
    }

} /// namespace INB::detail
