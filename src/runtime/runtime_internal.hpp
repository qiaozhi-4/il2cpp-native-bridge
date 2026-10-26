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

} /// namespace il2cpp_native_bridge::detail
