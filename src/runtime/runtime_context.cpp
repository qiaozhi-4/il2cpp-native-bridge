#include "runtime_internal.hpp"

#include <algorithm>
#include <sstream>

namespace il2cpp_native_bridge::detail {

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

    if (context->module == nullptr && !context->injected_api) {
        context->module = find_loaded_runtime_module();
        context->owns_module = context->module != nullptr;
        if (context->module == nullptr) {
            context->snapshot.state = RuntimeState::waiting_for_module;
            context->snapshot.last_error = "could not obtain a handle to loaded libil2cpp.so";
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
    }

    if (context->module != nullptr && !context->injected_api) {
        if (context->symbol_lookup_ready != nullptr && !context->symbol_lookup_ready(context->module)) {
            context->snapshot.state = RuntimeState::waiting_for_module;
            context->snapshot.last_error = "configured IL2CPP symbol resolver is not ready";
            context->snapshot.missing_apis.clear();
            return Result<void>::failure(make_error(ErrorCode::runtime_not_ready, context->snapshot.last_error));
        }
        const auto lookup = context->symbol_lookup == nullptr ? lookup_runtime_symbol : context->symbol_lookup;
        const auto exports = resolve_exports(context->module, lookup, context->export_overrides);
        context->api = exports.api;
        context->snapshot.missing_apis = exports.missing_required;
        context->snapshot.missing_optional_apis = exports.missing_optional;
        if (!exports.has_required_exports()) {
            std::ostringstream message;
            message << "missing required IL2CPP exports:";
            for (const auto& missing : exports.missing_required) {
                message << ' ' << missing;
            }
            context->snapshot.state = RuntimeState::unsupported;
            context->snapshot.last_error = message.str();
            return Result<void>::failure(make_error(ErrorCode::unsupported_runtime, context->snapshot.last_error));
        }
    }

    if (context->api.domain_get == nullptr || context->api.domain_get_assemblies == nullptr) {
        context->snapshot.state = RuntimeState::unsupported;
        context->snapshot.last_error = "runtime API table is incomplete";
        return Result<void>::failure(make_error(ErrorCode::unsupported_runtime, context->snapshot.last_error));
    }

    context->snapshot.state = RuntimeState::initializing;
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
    }
    context->snapshot.state = RuntimeState::ready;
    context->snapshot.last_error.clear();
    context->snapshot.missing_apis.clear();
    context->snapshot.missing_optional_apis.clear();
    context->snapshot.capabilities = {"class_lookup", "method_lookup", "field_access", "invocation", "thread_attachment"};
    if (context->api.image_get_class_count != nullptr && context->api.image_get_class != nullptr) {
        context->snapshot.capabilities.emplace_back("class_enumeration");
    }
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

void* find_cached_metadata(const std::shared_ptr<RuntimeContext>& context, std::string_view key) {
    if (context == nullptr) return nullptr;
    std::lock_guard lock(context->metadata_cache_mutex);
    const auto cached = context->metadata_cache.find(std::string(key));
    return cached == context->metadata_cache.end() ? nullptr : cached->second;
}

void cache_metadata(const std::shared_ptr<RuntimeContext>& context, std::string key, void* value) {
    if (context == nullptr || value == nullptr) return;
    std::lock_guard lock(context->metadata_cache_mutex);
    context->metadata_cache.insert_or_assign(std::move(key), value);
}

} /// namespace il2cpp_native_bridge::detail
