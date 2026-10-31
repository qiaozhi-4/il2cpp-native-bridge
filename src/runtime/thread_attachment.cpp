#include "runtime_internal.hpp"

namespace il2cpp_native_bridge::detail {

ThreadLease::ThreadLease(ThreadLease&& other) noexcept
    : context(std::move(other.context)), thread(other.thread), owns_attachment(other.owns_attachment) {
    other.thread = nullptr;
    other.owns_attachment = false;
}

ThreadLease& ThreadLease::operator=(ThreadLease&& other) noexcept {
    if (this != &other) {
        if (owns_attachment && thread != nullptr && context != nullptr) {
            const auto api = api_for(context);
            if (api.thread_detach != nullptr) api.thread_detach(thread);
        }
        context = std::move(other.context);
        thread = other.thread;
        owns_attachment = other.owns_attachment;
        other.thread = nullptr;
        other.owns_attachment = false;
    }
    return *this;
}

ThreadLease::~ThreadLease() {
    if (owns_attachment && thread != nullptr && context != nullptr) {
        const auto api = api_for(context);
        if (api.thread_detach != nullptr) {
            api.thread_detach(thread);
        }
    }
}

Result<ThreadLease> acquire_thread(const std::shared_ptr<RuntimeContext>& context) {
    auto ready = require_ready(context);
    if (!ready) return Result<ThreadLease>::failure(ready.error());

    const auto api = api_for(context);
    if (api.thread_current == nullptr || api.thread_attach == nullptr || api.thread_detach == nullptr) {
        return Result<ThreadLease>::failure(make_error(ErrorCode::missing_api, "thread attachment exports are unavailable"));
    }

    if (auto* current = api.thread_current(); current != nullptr) {
        return Result<ThreadLease>::success(ThreadLease{context, current, false});
    }

    Il2CppDomain* domain = nullptr;
    {
        std::lock_guard lock(context->mutex);
        domain = context->domain;
    }
    auto* attached = api.thread_attach(domain);
    if (attached == nullptr) {
        return Result<ThreadLease>::failure(make_error(ErrorCode::runtime_not_ready, "il2cpp_thread_attach returned null"));
    }
    return Result<ThreadLease>::success(ThreadLease{context, attached, true});
}

Result<ThreadLease> acquire_thread(const std::shared_ptr<RuntimeContext>& context,
                                   const Api& api,
                                   Il2CppDomain* domain) {
    if (context == nullptr) {
        return Result<ThreadLease>::failure(make_error(ErrorCode::internal_error, "runtime context is missing"));
    }
    if (api.thread_current == nullptr || api.thread_attach == nullptr || api.thread_detach == nullptr) {
        return Result<ThreadLease>::failure(make_error(ErrorCode::missing_api, "thread attachment exports are unavailable"));
    }

    /// 已附加的线程不归本次租约所有，作用域结束时不分离。
    if (auto* current = api.thread_current(); current != nullptr) {
        return Result<ThreadLease>::success(ThreadLease{context, current, false});
    }

    auto* attached = api.thread_attach(domain);
    if (attached == nullptr) {
        return Result<ThreadLease>::failure(make_error(ErrorCode::runtime_not_ready, "il2cpp_thread_attach returned null"));
    }
    return Result<ThreadLease>::success(ThreadLease{context, attached, true});
}

} /// namespace il2cpp_native_bridge::detail
