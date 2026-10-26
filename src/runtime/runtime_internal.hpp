#pragma once

#include "il2cpp_native_bridge/result.hpp"
#include "il2cpp_native_bridge/runtime_state.hpp"
#include "runtime/il2cpp_api.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace il2cpp_native_bridge::detail {

/// 类解析缓存上限。超限时整体清空，避免长驻进程按查询次数无界增长。
inline constexpr std::size_t kClassCacheLimit = 4096;
/// 方法与字段解析缓存上限，策略同上。
inline constexpr std::size_t kMetadataCacheLimit = 8192;

/// 类解析缓存条目。保留镜像名，使缓存命中与未命中两条路径返回一致的程序集名。
struct ClassCacheEntry {
    Il2CppClass* native{};
    std::string assembly_name;
};

struct RuntimeContext {
    mutable std::mutex mutex;
    mutable std::mutex metadata_cache_mutex;
    Api api;
    void* module{};
    bool owns_module{};
    Il2CppDomain* domain{};
    SymbolLookup symbol_lookup{};
    SymbolLookupReady symbol_lookup_ready{};
    RuntimeSnapshot snapshot;
    bool injected_api{};
    std::unordered_map<std::string, void*> metadata_cache;
    std::unordered_map<std::string, ClassCacheEntry> class_cache;
    std::unordered_map<std::string, void*> export_overrides;

    ~RuntimeContext();
};

struct ThreadLease {
    std::shared_ptr<RuntimeContext> context;
    Il2CppThread* thread{};
    bool owns_attachment{};

    ThreadLease() = default;
    ThreadLease(std::shared_ptr<RuntimeContext> owner, Il2CppThread* value, bool owns)
        : context(std::move(owner)), thread(value), owns_attachment(owns) {}
    ThreadLease(const ThreadLease&) = delete;
    ThreadLease& operator=(const ThreadLease&) = delete;
    ThreadLease(ThreadLease&& other) noexcept;
    ThreadLease& operator=(ThreadLease&& other) noexcept;
    ~ThreadLease();
};

void* find_loaded_runtime_module();
void close_runtime_module(void* module);
void* lookup_runtime_symbol(void* module, const char* name);

Result<void> refresh_context(const std::shared_ptr<RuntimeContext>& context);
Result<ThreadLease> acquire_thread(const std::shared_ptr<RuntimeContext>& context);
Result<void> require_ready(const std::shared_ptr<RuntimeContext>& context);
Api api_for(const std::shared_ptr<RuntimeContext>& context);
std::uint64_t generation_for(const std::shared_ptr<RuntimeContext>& context);
void* find_cached_metadata(const std::shared_ptr<RuntimeContext>& context, std::string_view key);
void cache_metadata(const std::shared_ptr<RuntimeContext>& context, std::string key, void* value);
bool find_cached_class(const std::shared_ptr<RuntimeContext>& context, std::string_view key, ClassCacheEntry& out);
void cache_class(const std::shared_ptr<RuntimeContext>& context,
                 std::string key,
                 Il2CppClass* native,
                 std::string assembly_name);

} /// namespace il2cpp_native_bridge::detail
