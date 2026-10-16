#include "metadata/metadata_internal.hpp"

namespace il2cpp_native_bridge::detail {

Result<InvokeOutcome> invoke_raw(const MethodHandle& method, Il2CppObject* instance, std::span<void*> arguments) {
    auto valid = validate_method_handle(method);
    if (!valid) return Result<InvokeOutcome>::failure(valid.error());
    if (!method.is_static && instance == nullptr) {
        return Result<InvokeOutcome>::failure(make_error(ErrorCode::invalid_instance, "instance method requires a managed object"));
    }
    auto attached = acquire_thread(method.declaring_class.context);
    if (!attached) return Result<InvokeOutcome>::failure(attached.error());

    const auto api = api_for(method.declaring_class.context);
    Il2CppObject* exception = nullptr;
    Il2CppObject* value = api.runtime_invoke(method.native,
        method.is_static ? nullptr : instance,
        arguments.empty() ? nullptr : arguments.data(), &exception);
    if (exception != nullptr) {
        return Result<InvokeOutcome>::failure(make_error(ErrorCode::managed_exception,
            "managed method raised an exception: " + method.name));
    }
    return Result<InvokeOutcome>::success(InvokeOutcome{value});
}

} /// namespace il2cpp_native_bridge::detail
