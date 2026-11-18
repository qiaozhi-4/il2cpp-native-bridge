#include "metadata_internal.hpp"

#include <cstddef>

namespace INB {

Result<std::size_t> Runtime::array_length(Il2CppObject* array) const {
    if (array == nullptr) {
        return Result<std::size_t>::failure(make_error(ErrorCode::invalid_argument,
                                                       "managed array must not be null"));
    }
    auto attached = detail::acquire_thread(context_);
    if (!attached) return Result<std::size_t>::failure(attached.error());

    const auto api = detail::api_for(context_);
    if (api.array_length != nullptr) {
        return Result<std::size_t>::success(api.array_length(reinterpret_cast<Il2CppArray*>(array)));
    }

    /// 导出缺失时走 System.Array 的反射属性；只读调用，不改变数组内容。
    auto length = klass("System.Array").method<std::int32_t>("get_Length").invoke_on(array);
    if (!length) return Result<std::size_t>::failure(length.error());
    if (length.value() < 0) {
        return Result<std::size_t>::failure(make_error(ErrorCode::invocation_failed,
                                                       "managed array returned a negative length"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(length.value()));
}

Result<void*> Runtime::unbox(Il2CppObject* boxed) const {
    if (boxed == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::invalid_argument,
                                                 "boxed value must not be null"));
    }
    auto attached = detail::acquire_thread(context_);
    if (!attached) return Result<void*>::failure(attached.error());

    const auto api = detail::api_for(context_);
    if (api.object_unbox == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::missing_api,
                                                 "il2cpp_object_unbox is unavailable"));
    }
    void* value = api.object_unbox(boxed);
    if (value == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::type_mismatch,
                                                 "managed value could not be unboxed"));
    }
    return Result<void*>::success(value);
}

} /// namespace INB
