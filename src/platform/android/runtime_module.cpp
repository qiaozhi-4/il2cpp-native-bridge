#include "runtime/runtime_internal.hpp"

#include "logging/internal_log.hpp"

#include <dlfcn.h>

#if defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__))
#include <link.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#endif

namespace INB::detail {

    #if defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__))
    namespace {

        /// 已加载的 runtime 模块：路径来自 dlpi_name，基址来自 dlpi_addr。
        struct RuntimeModule {
            std::string path;
            std::uintptr_t load_address{};
        };

        struct RuntimeModuleSearch {
            std::vector<RuntimeModule>* modules{};
        };

        /// dl_iterate_phdr 回调：挑出 runtime 模块，其余 so 直接跳过。
        int find_runtime_module(struct dl_phdr_info* info, std::size_t, void* user_data) {
            if (info == nullptr || info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') return 0;

            const std::string_view path(info->dlpi_name);
            const auto slash = path.find_last_of('/');
            const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        #if defined(__ANDROID__)
            if (name != "libil2cpp.so") return 0;
        #else
            if (name != "GameAssembly.so" && name != "libil2cpp.so") return 0;
        #endif

            auto* search = static_cast<RuntimeModuleSearch*>(user_data);
            search->modules->push_back({info->dlpi_name, static_cast<std::uintptr_t>(info->dlpi_addr)});
            return 0;
        }

        /// 用 il2cpp_domain_get 的实际映射基址核对句柄：dlopen 可能命中同名但不是同一份映射的 so。
        bool handle_matches_loaded_runtime(void* handle, const std::vector<RuntimeModule>& modules) {
            if (handle == nullptr) return false;
            void* domain_get = dlsym(handle, "il2cpp_domain_get");
            if (domain_get == nullptr) return false;

            Dl_info symbol_info{};
            if (dladdr(domain_get, &symbol_info) == 0 || symbol_info.dli_fbase == nullptr) return false;
            const auto symbol_load_address = reinterpret_cast<std::uintptr_t>(symbol_info.dli_fbase);
            for (const auto& module : modules) {
                if (module.load_address == symbol_load_address) return true;
            }
            return false;
        }

    } /// namespace
    #endif

    void* find_loaded_runtime_module() {
    #if defined(__ANDROID__)
    #if defined(RTLD_NOLOAD)
        std::vector<RuntimeModule> modules;
        RuntimeModuleSearch search{&modules};
        dl_iterate_phdr(find_runtime_module, &search);
        for (const auto& candidate : modules) {
            if (void* module = dlopen(candidate.path.c_str(), RTLD_NOW | RTLD_NOLOAD); module != nullptr) {
                if (handle_matches_loaded_runtime(module, modules)) {
                    INB_LOGI("IL2CPP module: %s (base %p)", candidate.path.c_str(),
                             reinterpret_cast<void*>(candidate.load_address));
                    return module;
                }
                dlclose(module);
            }
        }
        if (void* module = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD); module != nullptr) {
            if (handle_matches_loaded_runtime(module, modules)) {
                INB_LOGI("IL2CPP module: libil2cpp.so (handle %p)", module);
                return module;
            }
            dlclose(module);
        }
        return nullptr;
    #else
        return nullptr;
    #endif
    #elif defined(__linux__)
        std::vector<RuntimeModule> modules;
        RuntimeModuleSearch search{&modules};
        dl_iterate_phdr(find_runtime_module, &search);
    #if defined(RTLD_NOLOAD)
        for (const auto& candidate : modules) {
            if (void* module = dlopen(candidate.path.c_str(), RTLD_NOW | RTLD_NOLOAD); module != nullptr) {
                if (handle_matches_loaded_runtime(module, modules)) {
                    INB_LOGI("runtime module: %s (base %p)", candidate.path.c_str(),
                             reinterpret_cast<void*>(candidate.load_address));
                    return module;
                }
                dlclose(module);
            }
        }
        return nullptr;
    #else
        return nullptr;
    #endif
    #else
        return nullptr;
    #endif
    }

    void close_runtime_module(void* module) {
        if (module != nullptr) {
            dlclose(module);
        }
    }

    void* lookup_runtime_symbol(void* module, const char* name) {
        return module == nullptr || name == nullptr ? nullptr : dlsym(module, name);
    }

} /// namespace INB::detail
