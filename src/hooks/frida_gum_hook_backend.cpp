#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"

#include <frida-gum.h>

#include <mutex>
#include <utility>

namespace il2cpp_native_bridge {
namespace {

void initialize_gum() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { gum_init_embedded(); });
}

class FridaGumHookBackend final : public HookBackend {
public:
    FridaGumHookBackend() : interceptor_(gum_interceptor_obtain()) {}

    [[nodiscard]] bool available() const noexcept { return interceptor_ != nullptr; }

    ~FridaGumHookBackend() override {
        if (interceptor_ != nullptr) g_object_unref(interceptor_);
    }

    Result<void*> install(void* target, void* replacement) override {
        if (interceptor_ == nullptr || target == nullptr || replacement == nullptr) {
            return Result<void*>::failure(make_error(ErrorCode::hook_backend_unavailable,
                "Frida Gum interceptor, target, or replacement is missing"));
        }

        gpointer original = nullptr;
        const auto status = gum_interceptor_replace(
            interceptor_, target, replacement, &original, nullptr);
        if (status != GUM_REPLACE_OK || original == nullptr) {
            return Result<void*>::failure(make_error(ErrorCode::hook_install_failed,
                "Frida Gum rejected Interceptor.replace with status " + std::to_string(status)));
        }
        return Result<void*>::success(original);
    }

    Result<void> remove(void* target) override {
        if (interceptor_ == nullptr || target == nullptr) {
            return Result<void>::failure(make_error(ErrorCode::hook_backend_unavailable,
                "Frida Gum interceptor or target is missing"));
        }

        gum_interceptor_revert(interceptor_, target);
        if (!gum_interceptor_flush_function(interceptor_, target)) {
            return Result<void>::failure(make_error(ErrorCode::hook_install_failed,
                "Frida Gum could not flush the reverted Hook"));
        }
        return Result<void>::success();
    }

private:
    GumInterceptor* interceptor_{};
};

} /// namespace

std::shared_ptr<HookBackend> make_frida_gum_hook_backend() {
    initialize_gum();
    auto backend = std::make_shared<FridaGumHookBackend>();
    if (!backend->available()) return {};
    return backend;
}

Result<void> with_frida_gum_hook_backend(
    const std::function<Result<void>(const std::shared_ptr<HookBackend>&)>& action) {
    if (!action) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "Frida Gum Hook backend action is empty"));
    }
    const auto backend = make_frida_gum_hook_backend();
    if (!backend) {
        return Result<void>::failure(make_error(ErrorCode::hook_backend_unavailable,
            "Frida Gum interceptor is unavailable"));
    }
    return action(backend);
}

Result<void> perform_when_ready_with_frida_gum_hook_backend(
    const Runtime& runtime,
    std::function<Result<void>(const std::shared_ptr<HookBackend>&)> action,
    std::function<void(Result<void>)> completion,
    RuntimeWaitOptions options) {
    if (!action) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "Frida Gum Hook backend action is empty"));
    }

    return runtime.perform_when_ready(
        [action = std::move(action)] {
            return with_frida_gum_hook_backend(action);
        },
        std::move(completion), std::move(options));
}

} /// namespace il2cpp_native_bridge
