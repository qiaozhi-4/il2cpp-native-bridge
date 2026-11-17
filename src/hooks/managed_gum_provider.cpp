#include "hooks/managed_provider_internal.hpp"

#include <frida-gum.h>

#include <mutex>

namespace INB::detail {
namespace {

/**
 * 取共享 interceptor。
 *
 * Gum 必须先 `gum_init_embedded()` 再 `gum_interceptor_obtain()`：
 * 直接 obtain 会走未初始化的 GType 注册路径并在设备上崩溃（T044 实测）。
 */
GumInterceptor* interceptor() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { gum_init_embedded(); });
    return gum_interceptor_obtain();
}

Result<void*> gum_install(void* target, void* replacement, void* data) {
    GumInterceptor* shared = interceptor();
    if (shared == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::hook_backend_unavailable,
                                                 "Gum interceptor is unavailable"));
    }

    GumReplaceOptions options{};
    options.replacement_data = data;
    void* original = nullptr;
    const GumReplaceReturn replaced =
            gum_interceptor_replace(shared, target, replacement, &original, &options);
    if (replaced != GUM_REPLACE_OK) {
        return Result<void*>::failure(make_error(ErrorCode::hook_install_failed,
                                                 "gum_interceptor_replace rejected the target"));
    }
    if (original == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::hook_install_failed,
                                                 "Gum returned a null original trampoline"));
    }
    return Result<void*>::success(original);
}

Result<void> gum_revert(void* target) {
    GumInterceptor* shared = interceptor();
    if (shared == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::hook_backend_unavailable,
                                                "Gum interceptor is unavailable"));
    }
    gum_interceptor_revert(shared, target);
    return Result<void>::success();
}

bool gum_flush(void* target) {
    GumInterceptor* shared = interceptor();
    return shared != nullptr && gum_interceptor_flush_function(shared, target) != 0;
}

const ReplacementRecordHeader* gum_current_record() {
    GumInvocationContext* context = gum_interceptor_get_current_invocation();
    if (context == nullptr) return nullptr;
    return static_cast<const ReplacementRecordHeader*>(gum_invocation_context_get_replacement_data(context));
}

const ManagedGumProvider kProvider{true, gum_install, gum_revert, gum_flush, gum_current_record};

} /// namespace

const ManagedGumProvider& managed_gum_provider() { return kProvider; }

} /// namespace INB::detail
