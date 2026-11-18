#pragma once

#include "il2cpp_native_bridge/result.hpp"
#include "il2cpp_native_bridge/runtime_state.hpp"
#include "runtime/il2cpp_api.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace INB::detail {

/// 类解析缓存上限。超限时整体清空，避免长驻进程按查询次数无界增长。
inline constexpr std::size_t kClassCacheLimit = 4096;
/// 方法与字段解析缓存上限，策略同上。
inline constexpr std::size_t kMetadataCacheLimit = 8192;

/// 类解析缓存条目。保留镜像名，使缓存命中与未命中两条路径返回一致的程序集名。
struct ClassCacheEntry {
    Il2CppClass* native{};
    std::string assembly_name;
};

class FacadeExecutor;

/**
 * 新门面的就绪投影。
 *
 * 与旧 `RuntimeSnapshot` 独立：core 就绪只要求安全就绪检测、domain、程序集与线程 API，
 * invoke、字段、类型识别等能力按操作单独判断。两者共享同一 module/api/domain/generation。
 */
struct CoreSnapshot {
    RuntimeState state{RuntimeState::waiting_for_module};
    std::string last_error;
    /// 缺失的核心导出；非空即 unsupported。
    std::vector<std::string> missing_apis;
    /// 当前可用能力分组，供诊断使用。
    std::vector<std::string> capabilities;
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
    CoreSnapshot core;
    /// 缺失的可选导出按能力分组；非注入式解析时填充，用于按需失败诊断。
    std::unordered_map<std::string, std::vector<std::string>> missing_by_group;
    bool injected_api{};
    std::unordered_map<std::string, void*> metadata_cache;
    std::unordered_map<std::string, ClassCacheEntry> class_cache;
    std::unordered_map<std::string, void*> export_overrides;
    /// 导出表是否已打印过解析日志。轮询重试会反复解析，日志只保留第一次。
    bool exports_logged{};
    /// 已打印过的状态迁移日志点，取值见 runtime_context.cpp 的 LogPoint。
    unsigned logged_points{};
    /// 已打印过解析日志的查询键；与元数据缓存共用锁与上限策略，避免逐帧查询刷屏。
    std::unordered_set<std::string> logged_lookups;
    /// 门面协调执行器；由 context 独占持有，worker 只借用 context。
    std::unique_ptr<FacadeExecutor> facade_executor;

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

/// 定位并持有已加载的 IL2CPP 模块，返回可供 dlsym 的句柄。
void* find_loaded_runtime_module();
/// 关闭由 find_loaded_runtime_module 取得的模块句柄。
void close_runtime_module(void* module);
/// 在模块中按名查找导出符号。
void* lookup_runtime_symbol(void* module, const char* name);

/// 刷新 context 的旧门面就绪投影。
Result<void> refresh_context(const std::shared_ptr<RuntimeContext>& context);
/// 建立新门面的 core 就绪投影；不要求 invoke/字段等非核心导出。
Result<void> refresh_core(const std::shared_ptr<RuntimeContext>& context);
/// 附加当前线程到 runtime；已附加时不重复附加。
Result<ThreadLease> acquire_thread(const std::shared_ptr<RuntimeContext>& context);
/// 使用已校验的就绪视图附加线程；不再重复执行 strict-ready 判定。
Result<ThreadLease> acquire_thread(const std::shared_ptr<RuntimeContext>& context, const Api& api, Il2CppDomain* domain);
/// 要求旧门面投影处于 ready，否则返回未就绪错误。
Result<void> require_ready(const std::shared_ptr<RuntimeContext>& context);
/// 读取当前导出表；context 为空时返回空表。
Api api_for(const std::shared_ptr<RuntimeContext>& context);
/// 读取当前 runtime 代际；context 为空时返回 0。
std::uint64_t generation_for(const std::shared_ptr<RuntimeContext>& context);

/// 仅供测试：构造未就绪的 context，调用方可自行配置 module、symbol_lookup 或 injected_api。
std::shared_ptr<RuntimeContext> make_testing_context(Api api, Il2CppDomain* domain);
/// 创建空的运行时 context；构造点集中在 runtime 层，其他翻译单元不依赖门面类型。
std::shared_ptr<RuntimeContext> make_runtime_context();
/// 生产进程唯一的运行时 context；Runtime 与新门面共享它，以便符号解析器等配置只做一次。
std::shared_ptr<RuntimeContext> current_runtime_context();
/// 按 key 查元数据缓存；未命中或 context 为空时返回空指针。
void* find_cached_metadata(const std::shared_ptr<RuntimeContext>& context, std::string_view key);
/// 写入元数据缓存；达到上限时整体清空。
void cache_metadata(const std::shared_ptr<RuntimeContext>& context, std::string key, void* value);
/// 按 key 查类缓存，命中时填入 out 并返回 true。
bool find_cached_class(const std::shared_ptr<RuntimeContext>& context, std::string_view key, ClassCacheEntry& out);
/// 写入类缓存；达到上限时整体清空。
void cache_class(const std::shared_ptr<RuntimeContext>& context,
                 std::string key,
                 Il2CppClass* native,
                 std::string assembly_name);
/**
 * 首次见到该查询键时返回 true，供「每个查询只打印一次解析日志」使用。
 *
 * 键沿用元数据缓存的键格式，因此同一类/方法/字段在缓存被清空后也不会重复打印。
 *
 * @param context 运行时 context；为空时返回 false。
 * @param key 查询键。
 * @return 该键此前未出现过时为 true。
 */
bool mark_lookup_logged(const std::shared_ptr<RuntimeContext>& context, std::string_view key);

} /// namespace INB::detail
