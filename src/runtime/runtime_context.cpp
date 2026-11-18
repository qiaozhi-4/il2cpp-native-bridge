#include "runtime_internal.hpp"
#include "facade/facade_internal.hpp"
#include "logging/internal_log.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace INB::detail {
namespace {

/// 导出解析阶段的结果：只有 ready 才能继续判断就绪。
enum class Discovery {
    ready,
    waiting_for_module,
    resolver_not_ready,
};

/// 状态迁移日志点。每个点在一次运行里只打印一次，轮询重试不会把同一行刷满 logcat。
enum class LogPoint : unsigned {
    waiting_for_module = 1u << 0,
    resolver_not_ready = 1u << 1,
    unsupported = 1u << 2,
    initializing = 1u << 3,
    ready = 1u << 4,
};

[[maybe_unused]] const char* state_name(RuntimeState state) {
    switch (state) {
        case RuntimeState::waiting_for_module: return "waiting_for_module";
        case RuntimeState::initializing: return "initializing";
        case RuntimeState::ready: return "ready";
        case RuntimeState::unsupported: return "unsupported";
    }
    return "unknown";
}

/**
 * 首次到达某个日志点时打印状态；detail 以回调延迟构造，轨迹关闭时连字符串都不会生成。
 *
 * unsupported 始终记录；其余状态只在轨迹构建输出。
 *
 * @param context 当前运行时 context。
 * @param point 迁移日志点。
 * @param state 该日志点对应的状态，决定日志级别。
 * @param detail 描述该次迁移的文本构造器。
 */
template <typename Detail>
void log_state_once(RuntimeContext& context, LogPoint point, RuntimeState state, Detail detail) {
    const bool is_error = state == RuntimeState::unsupported;
    if constexpr (!INB_LOG_ENABLED) {
        /// 只留错误的构建里，其余状态连去重和文本构造都省掉。
        if (!is_error) return;
    }

    const auto bit = static_cast<unsigned>(point);
    if ((context.logged_points & bit) != 0) return;
    context.logged_points |= bit;

    const std::string message = detail();
    if (is_error) {
        INB_LOGE("runtime unsupported: %s", message.c_str());
        return;
    }
    if constexpr (INB_LOG_ENABLED) {
        if (state == RuntimeState::ready) {
            INB_LOGI("runtime ready: %s", message.c_str());
        } else {
            INB_LOGD("runtime %s: %s", state_name(state), message.c_str());
        }
    }
}

/// 解析模块与导出表；调用方持有 context.mutex。两个就绪投影共用这一次解析。
Discovery discover_exports(RuntimeContext& context) {
    if (context.module == nullptr && !context.injected_api) {
        context.module = find_loaded_runtime_module();
        context.owns_module = context.module != nullptr;
        if (context.module == nullptr) return Discovery::waiting_for_module;
    }

    if (context.module != nullptr && !context.injected_api) {
        if (context.symbol_lookup_ready != nullptr && !context.symbol_lookup_ready(context.module)) {
            return Discovery::resolver_not_ready;
        }
        const auto lookup = context.symbol_lookup == nullptr ? lookup_runtime_symbol : context.symbol_lookup;
        /// 逐符号日志只在第一次解析时打印：未就绪前的轮询会反复走到这里。
        auto exports = resolve_exports(context.module, lookup, context.export_overrides, !context.exports_logged);
        context.exports_logged = true;
        context.api = exports.api;
        context.snapshot.missing_apis = exports.missing_required;
        context.snapshot.missing_optional_apis = exports.missing_optional;
        context.missing_by_group = std::move(exports.missing_by_group);
    }
    return Discovery::ready;
}

/// 按导出清单推导当前可用的能力分组，避免另写一份成员与分组的对应关系。
std::vector<std::string> available_capabilities(const Api& api) {
    std::vector<std::string> capabilities{"readiness", "thread_attachment"};
    if (api.runtime_invoke != nullptr && api.object_unbox != nullptr) capabilities.emplace_back("invocation");

    std::set<std::string> groups{
            capability_group::kEnumeration,
            capability_group::kAllocation,
            capability_group::kType,
            capability_group::kInstance,
            capability_group::kString,
    };

#define IL2CPP_CAPABILITY_REQUIRED(member, symbol) (void) 0;
#define IL2CPP_CAPABILITY_OPTIONAL(member, symbol, group) \
    if (api.member == nullptr) groups.erase(group);

    IL2CPP_NATIVE_BRIDGE_EXPORT_LIST(IL2CPP_CAPABILITY_REQUIRED, IL2CPP_CAPABILITY_OPTIONAL)

#undef IL2CPP_CAPABILITY_REQUIRED
#undef IL2CPP_CAPABILITY_OPTIONAL

    capabilities.insert(capabilities.end(), groups.begin(), groups.end());
    return capabilities;
}

std::string describe_missing(const char* prefix, const std::vector<std::string>& missing) {
    std::ostringstream message;
    message << prefix;
    for (const auto& name : missing) message << ' ' << name;
    return message.str();
}

} /// namespace

RuntimeContext::~RuntimeContext() {
    if (owns_module && module != nullptr) {
        close_runtime_module(module);
    }
}

Result<void> refresh_context(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::internal_error, "runtime context is missing"));
    }

    std::lock_guard lock(context->mutex);
    if (context->snapshot.state == RuntimeState::ready && context->domain != nullptr) {
        return Result<void>::success();
    }

    switch (discover_exports(*context)) {
        case Discovery::waiting_for_module:
            context->snapshot.state = RuntimeState::waiting_for_module;
            context->snapshot.last_error = "could not obtain a handle to loaded libil2cpp.so";
            log_state_once(*context, LogPoint::waiting_for_module, RuntimeState::waiting_for_module,
                           [] { return std::string("libil2cpp.so is not loaded yet"); });
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        case Discovery::resolver_not_ready:
            context->snapshot.state = RuntimeState::waiting_for_module;
            context->snapshot.last_error = "configured IL2CPP symbol resolver is not ready";
            context->snapshot.missing_apis.clear();
            log_state_once(*context, LogPoint::resolver_not_ready, RuntimeState::waiting_for_module,
                           [] { return std::string("the configured symbol resolver is not ready"); });
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        case Discovery::ready:
            break;
    }

    if (!context->snapshot.missing_apis.empty()) {
        context->snapshot.state = RuntimeState::unsupported;
        context->snapshot.last_error = describe_missing("missing required IL2CPP exports:",
                                                        context->snapshot.missing_apis);
        log_state_once(*context, LogPoint::unsupported, RuntimeState::unsupported,
                       [&context] { return context->snapshot.last_error; });
        return Result<void>::failure(make_error(ErrorCode::unsupported_runtime, context->snapshot.last_error));
    }

    if (context->api.domain_get == nullptr || context->api.domain_get_assemblies == nullptr) {
        context->snapshot.state = RuntimeState::unsupported;
        context->snapshot.last_error = "runtime API table is incomplete";
        log_state_once(*context, LogPoint::unsupported, RuntimeState::unsupported,
                       [&context] { return context->snapshot.last_error; });
        return Result<void>::failure(make_error(ErrorCode::unsupported_runtime, context->snapshot.last_error));
    }

    context->snapshot.state = RuntimeState::initializing;
    log_state_once(*context, LogPoint::initializing, RuntimeState::initializing,
                   [] { return std::string("exports are complete, resolving the IL2CPP domain"); });
    if (context->domain == nullptr) {
        if (context->api.get_corlib != nullptr && context->api.get_corlib() == nullptr) {
            context->snapshot.last_error = "il2cpp_get_corlib returned null; IL2CPP runtime is not ready";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
        if (context->api.get_corlib == nullptr &&
            (context->api.thread_current == nullptr || context->api.thread_current() == nullptr)) {
            context->snapshot.last_error = "il2cpp_get_corlib is unavailable and the current native thread is not attached to IL2CPP";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
        context->domain = context->api.domain_get();
        if (context->domain == nullptr) {
            context->snapshot.last_error = "il2cpp_domain_get returned null after the IL2CPP runtime readiness check";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
    }

    std::size_t assembly_count = 0;
    Il2CppThread* attached_thread = nullptr;
    if (context->api.thread_current != nullptr && context->api.thread_current() == nullptr) {
        attached_thread = context->api.thread_attach(context->domain);
        if (attached_thread == nullptr) {
            context->snapshot.last_error = "il2cpp_thread_attach failed while checking loaded assemblies";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
    }
    const auto** assemblies = context->api.domain_get_assemblies(context->domain, &assembly_count);
    if (attached_thread != nullptr) context->api.thread_detach(attached_thread);
    if (assemblies == nullptr || assembly_count == 0) {
        context->snapshot.last_error = "IL2CPP domain has no loaded assemblies yet";
        return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
    }

    if (context->snapshot.generation == 0) {
        ++context->snapshot.generation;
        std::lock_guard cache_lock(context->metadata_cache_mutex);
        context->metadata_cache.clear();
        context->class_cache.clear();
    }
    context->snapshot.state = RuntimeState::ready;
    context->snapshot.last_error.clear();
    context->snapshot.missing_apis.clear();
    context->snapshot.missing_optional_apis.clear();
    context->snapshot.capabilities = {"class_lookup", "method_lookup", "field_access", "invocation", "thread_attachment"};
    if (context->api.image_get_class_count != nullptr && context->api.image_get_class != nullptr) {
        context->snapshot.capabilities.emplace_back("class_enumeration");
    }
    log_state_once(*context, LogPoint::ready, RuntimeState::ready, [&context, assembly_count] {
        return "generation=" + std::to_string(context->snapshot.generation) +
               " assemblies=" + std::to_string(assembly_count);
    });
    return Result<void>::success();
}

Result<void> refresh_core(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::internal_error, "runtime context is missing"));
    }

    std::lock_guard lock(context->mutex);
    if (context->core.state == RuntimeState::ready && context->domain != nullptr) {
        return Result<void>::success();
    }

    switch (discover_exports(*context)) {
        case Discovery::waiting_for_module:
            context->core.state = RuntimeState::waiting_for_module;
            context->core.last_error = "could not obtain a handle to loaded libil2cpp.so";
            log_state_once(*context, LogPoint::waiting_for_module, RuntimeState::waiting_for_module,
                           [] { return std::string("libil2cpp.so is not loaded yet"); });
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
        case Discovery::resolver_not_ready:
            context->core.state = RuntimeState::waiting_for_module;
            context->core.last_error = "configured IL2CPP symbol resolver is not ready";
            log_state_once(*context, LogPoint::resolver_not_ready, RuntimeState::waiting_for_module,
                           [] { return std::string("the configured symbol resolver is not ready"); });
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
        case Discovery::ready:
            break;
    }

    /// 默认发现要求 get_corlib 可用：它既是初始化判定的唯一安全依据，也决定是否需要 attach。
    std::vector<std::string> missing_core;
    if (context->api.get_corlib == nullptr) missing_core.emplace_back("il2cpp_get_corlib");
    if (context->api.domain_get == nullptr) missing_core.emplace_back("il2cpp_domain_get");
    if (context->api.domain_get_assemblies == nullptr) missing_core.emplace_back("il2cpp_domain_get_assemblies");
    if (context->api.thread_current == nullptr) missing_core.emplace_back("il2cpp_thread_current");
    if (context->api.thread_attach == nullptr) missing_core.emplace_back("il2cpp_thread_attach");
    if (context->api.thread_detach == nullptr) missing_core.emplace_back("il2cpp_thread_detach");
    if (!missing_core.empty()) {
        context->core.missing_apis = std::move(missing_core);
        context->core.state = RuntimeState::unsupported;
        context->core.last_error = describe_missing("missing core IL2CPP exports:", context->core.missing_apis);
        log_state_once(*context, LogPoint::unsupported, RuntimeState::unsupported,
                       [&context] { return context->core.last_error; });
        return Result<void>::failure(make_error(ErrorCode::unsupported_runtime, context->core.last_error));
    }
    context->core.missing_apis.clear();

    context->core.state = RuntimeState::initializing;
    log_state_once(*context, LogPoint::initializing, RuntimeState::initializing,
                   [] { return std::string("core exports are complete, resolving the IL2CPP domain"); });
    if (context->domain == nullptr) {
        if (context->api.get_corlib() == nullptr) {
            context->core.last_error = "il2cpp_get_corlib returned null; IL2CPP runtime is not ready";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
        }
        context->domain = context->api.domain_get();
        if (context->domain == nullptr) {
            context->core.last_error = "il2cpp_domain_get returned null after the IL2CPP runtime readiness check";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
        }
    }

    std::size_t assembly_count = 0;
    Il2CppThread* attached_thread = nullptr;
    if (context->api.thread_current() == nullptr) {
        attached_thread = context->api.thread_attach(context->domain);
        if (attached_thread == nullptr) {
            context->core.last_error = "il2cpp_thread_attach failed while checking loaded assemblies";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
        }
    }
    const auto** assemblies = context->api.domain_get_assemblies(context->domain, &assembly_count);
    if (attached_thread != nullptr) context->api.thread_detach(attached_thread);
    if (assemblies == nullptr || assembly_count == 0) {
        context->core.last_error = "IL2CPP domain has no loaded assemblies yet";
        return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->core.last_error));
    }

    /// generation 由两个投影共享：先就绪的一方建立，另一方不再提升。
    if (context->snapshot.generation == 0) {
        ++context->snapshot.generation;
        std::lock_guard cache_lock(context->metadata_cache_mutex);
        context->metadata_cache.clear();
        context->class_cache.clear();
    }
    context->core.state = RuntimeState::ready;
    context->core.last_error.clear();
    context->core.capabilities = available_capabilities(context->api);
    log_state_once(*context, LogPoint::ready, RuntimeState::ready, [&context, assembly_count] {
        return "generation=" + std::to_string(context->snapshot.generation) +
               " assemblies=" + std::to_string(assembly_count);
    });
    return Result<void>::success();
}

Result<void> require_ready(const std::shared_ptr<RuntimeContext>& context) {
    return refresh_context(context);
}

Api api_for(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) return {};
    std::lock_guard lock(context->mutex);
    return context->api;
}

std::uint64_t generation_for(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) return 0;
    std::lock_guard lock(context->mutex);
    return context->snapshot.generation;
}

std::shared_ptr<RuntimeContext> make_runtime_context() {
    return std::make_shared<RuntimeContext>();
}

std::shared_ptr<RuntimeContext> make_testing_context(Api api, Il2CppDomain* domain) {
    auto context = make_runtime_context();
    context->api = api;
    context->domain = domain;
    return context;
}

void* find_cached_metadata(const std::shared_ptr<RuntimeContext>& context, std::string_view key) {
    if (context == nullptr) return nullptr;
    std::lock_guard lock(context->metadata_cache_mutex);
    const auto cached = context->metadata_cache.find(std::string(key));
    return cached == context->metadata_cache.end() ? nullptr : cached->second;
}

void cache_metadata(const std::shared_ptr<RuntimeContext>& context, std::string key, void* value) {
    if (context == nullptr || value == nullptr) return;
    std::lock_guard lock(context->metadata_cache_mutex);
    if (context->metadata_cache.size() >= kMetadataCacheLimit) context->metadata_cache.clear();
    context->metadata_cache.insert_or_assign(std::move(key), value);
}

bool find_cached_class(const std::shared_ptr<RuntimeContext>& context, std::string_view key, ClassCacheEntry& out) {
    if (context == nullptr) return false;
    std::lock_guard lock(context->metadata_cache_mutex);
    const auto cached = context->class_cache.find(std::string(key));
    if (cached == context->class_cache.end()) return false;
    out = cached->second;
    return true;
}

void cache_class(const std::shared_ptr<RuntimeContext>& context,
                 std::string key,
                 Il2CppClass* native,
                 std::string assembly_name) {
    if (context == nullptr || native == nullptr) return;
    std::lock_guard lock(context->metadata_cache_mutex);
    if (context->class_cache.size() >= kClassCacheLimit) context->class_cache.clear();
    context->class_cache.insert_or_assign(std::move(key), ClassCacheEntry{native, std::move(assembly_name)});
}

bool mark_lookup_logged(const std::shared_ptr<RuntimeContext>& context, std::string_view key) {
    if (context == nullptr) return false;
    std::lock_guard lock(context->metadata_cache_mutex);
    if (context->logged_lookups.size() >= kMetadataCacheLimit) context->logged_lookups.clear();
    return context->logged_lookups.emplace(key).second;
}

} /// namespace INB::detail
