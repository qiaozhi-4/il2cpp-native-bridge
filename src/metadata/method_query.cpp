#include "metadata_internal.hpp"
#include "hooks/hook_internal.hpp"

#include <algorithm>
#include <cstring>

namespace il2cpp_native_bridge::detail {

Result<std::vector<MethodMetadata>> resolve_methods(const ClassHandle& klass) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<std::vector<MethodMetadata>>::failure(valid.error());
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<std::vector<MethodMetadata>>::failure(attached.error());

    const auto api = api_for(klass.context);
    std::vector<MethodMetadata> methods;
    void* iterator = nullptr;
    while (const auto* method = api.class_get_methods(klass.native, &iterator)) {
        const char* name = api.method_get_name(method);
        if (name == nullptr) continue;

        auto return_type = managed_name(api, api.method_get_return_type(method));
        if (!return_type) return Result<std::vector<MethodMetadata>>::failure(return_type.error());

        const auto parameter_count = api.method_get_param_count(method);
        std::vector<std::string> parameter_types;
        parameter_types.reserve(parameter_count);
        for (std::uint32_t index = 0; index < parameter_count; ++index) {
            auto parameter_type = managed_name(api, api.method_get_param(method, index));
            if (!parameter_type) return Result<std::vector<MethodMetadata>>::failure(parameter_type.error());
            parameter_types.push_back(parameter_type.take_value());
        }

        std::uint32_t implementation_flags = 0;
        const auto flags = api.method_get_flags(method, &implementation_flags);
        methods.push_back(MethodMetadata{
            .name = name,
            .return_type = return_type.take_value(),
            .parameter_types = std::move(parameter_types),
            .is_static = (flags & kMethodAttributeStatic) != 0,
        });
    }
    std::sort(methods.begin(), methods.end(), [](const MethodMetadata& left, const MethodMetadata& right) {
        if (left.name != right.name) return left.name < right.name;
        if (left.parameter_types != right.parameter_types) return left.parameter_types < right.parameter_types;
        if (left.return_type != right.return_type) return left.return_type < right.return_type;
        return left.is_static < right.is_static;
    });
    return Result<std::vector<MethodMetadata>>::success(std::move(methods));
}

Result<MethodHandle> resolve_method(const ClassHandle& klass,
                                    std::string_view name,
                                    std::string_view return_type,
                                    const std::vector<std::string>& parameter_types) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<MethodHandle>::failure(valid.error());
    if (name.empty()) {
        return Result<MethodHandle>::failure(make_error(ErrorCode::invalid_argument, "method name is empty"));
    }
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<MethodHandle>::failure(attached.error());

    const auto api = api_for(klass.context);
    auto cache_key = metadata_cache_key("method", klass.generation, klass.native);
    append_metadata_cache_part(cache_key, name);
    append_metadata_cache_part(cache_key, return_type);
    append_metadata_cache_part(cache_key, std::to_string(parameter_types.size()));
    for (const auto& parameter_type : parameter_types) append_metadata_cache_part(cache_key, parameter_type);
    if (auto* cached = find_cached_metadata(klass.context, cache_key); cached != nullptr) {
        auto* method = static_cast<const Il2CppMethodInfo*>(cached);
        std::uint32_t implementation_flags = 0;
        const auto flags = api.method_get_flags(method, &implementation_flags);
        MethodHandle result;
        result.declaring_class = klass;
        result.native = method;
        result.name = std::string(name);
        result.return_type = std::string(return_type);
        result.parameter_types = parameter_types;
        result.is_static = (flags & kMethodAttributeStatic) != 0;
        return Result<MethodHandle>::success(std::move(result));
    }
    const Il2CppMethodInfo* match = nullptr;
    std::size_t named_matches = 0;
    std::size_t exact_matches = 0;
    void* iterator = nullptr;
    while (const auto* method = api.class_get_methods(klass.native, &iterator)) {
        const char* method_name = api.method_get_name(method);
        if (method_name == nullptr || name != method_name) continue;
        ++named_matches;
        if (api.method_get_param_count(method) != parameter_types.size()) continue;

        const auto actual_return_type = managed_name(api, api.method_get_return_type(method));
        if (!actual_return_type || actual_return_type.value() != return_type) continue;

        bool parameters_match = true;
        for (std::size_t index = 0; index < parameter_types.size(); ++index) {
            const auto actual_type = managed_name(api, api.method_get_param(method, static_cast<std::uint32_t>(index)));
            if (!actual_type || actual_type.value() != parameter_types[index]) {
                parameters_match = false;
                break;
            }
        }
        if (!parameters_match) continue;
        match = method;
        ++exact_matches;
    }

    if (exact_matches == 0) {
        const auto code = named_matches == 0 ? ErrorCode::not_found : ErrorCode::type_mismatch;
        return Result<MethodHandle>::failure(make_error(code,
            named_matches == 0 ? "managed method was not found: " + std::string(name)
                               : "managed method signature did not match: " + std::string(name)));
    }
    if (exact_matches > 1) {
        return Result<MethodHandle>::failure(make_error(ErrorCode::ambiguous_match,
            "managed method signature matched multiple methods: " + std::string(name)));
    }

    std::uint32_t implementation_flags = 0;
    const auto flags = api.method_get_flags(match, &implementation_flags);
    MethodHandle result;
    result.declaring_class = klass;
    result.native = match;
    result.name = std::string(name);
    result.return_type = std::string(return_type);
    result.parameter_types = parameter_types;
    result.is_static = (flags & kMethodAttributeStatic) != 0;
    cache_metadata(klass.context, std::move(cache_key), const_cast<Il2CppMethodInfo*>(match));
    return Result<MethodHandle>::success(std::move(result));
}

Result<MethodHandle> resolve_method_by_arity(const ClassHandle& klass,
                                             std::string_view name,
                                             std::size_t parameter_count) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<MethodHandle>::failure(valid.error());
    if (name.empty()) {
        return Result<MethodHandle>::failure(make_error(ErrorCode::invalid_argument, "method name is empty"));
    }
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<MethodHandle>::failure(attached.error());

    const auto api = api_for(klass.context);
    auto cache_key = metadata_cache_key("method_arity", klass.generation, klass.native);
    append_metadata_cache_part(cache_key, name);
    append_metadata_cache_part(cache_key, std::to_string(parameter_count));
    if (auto* cached = find_cached_metadata(klass.context, cache_key); cached != nullptr) {
        auto* method = static_cast<const Il2CppMethodInfo*>(cached);
        auto return_type = managed_name(api, api.method_get_return_type(method));
        if (!return_type) return Result<MethodHandle>::failure(return_type.error());
        std::vector<std::string> parameter_types;
        parameter_types.reserve(parameter_count);
        for (std::size_t index = 0; index < parameter_count; ++index) {
            auto parameter_type = managed_name(api, api.method_get_param(method, static_cast<std::uint32_t>(index)));
            if (!parameter_type) return Result<MethodHandle>::failure(parameter_type.error());
            parameter_types.push_back(parameter_type.take_value());
        }
        std::uint32_t implementation_flags = 0;
        const auto flags = api.method_get_flags(method, &implementation_flags);
        MethodHandle result;
        result.declaring_class = klass;
        result.native = method;
        result.name = std::string(name);
        result.return_type = return_type.take_value();
        result.parameter_types = std::move(parameter_types);
        result.is_static = (flags & kMethodAttributeStatic) != 0;
        return Result<MethodHandle>::success(std::move(result));
    }
    const Il2CppMethodInfo* match = nullptr;
    std::size_t named_matches = 0;
    std::size_t arity_matches = 0;
    void* iterator = nullptr;
    while (const auto* method = api.class_get_methods(klass.native, &iterator)) {
        const char* method_name = api.method_get_name(method);
        if (method_name == nullptr || name != method_name) continue;
        ++named_matches;
        if (api.method_get_param_count(method) != parameter_count) continue;
        match = method;
        ++arity_matches;
    }

    if (arity_matches == 0) {
        const auto code = named_matches == 0 ? ErrorCode::not_found : ErrorCode::type_mismatch;
        return Result<MethodHandle>::failure(make_error(code,
            named_matches == 0 ? "managed method was not found: " + std::string(name)
                               : "managed method parameter count did not match: " + std::string(name)));
    }
    if (arity_matches > 1) {
        return Result<MethodHandle>::failure(make_error(ErrorCode::ambiguous_match,
            "managed method name and parameter count matched multiple methods: " + std::string(name)));
    }

    auto return_type = managed_name(api, api.method_get_return_type(match));
    if (!return_type) return Result<MethodHandle>::failure(return_type.error());
    std::vector<std::string> parameter_types;
    parameter_types.reserve(parameter_count);
    for (std::size_t index = 0; index < parameter_count; ++index) {
        auto parameter_type = managed_name(api, api.method_get_param(match, static_cast<std::uint32_t>(index)));
        if (!parameter_type) return Result<MethodHandle>::failure(parameter_type.error());
        parameter_types.push_back(parameter_type.take_value());
    }

    std::uint32_t implementation_flags = 0;
    const auto flags = api.method_get_flags(match, &implementation_flags);
    MethodHandle result;
    result.declaring_class = klass;
    result.native = match;
    result.name = std::string(name);
    result.return_type = return_type.take_value();
    result.parameter_types = std::move(parameter_types);
    result.is_static = (flags & kMethodAttributeStatic) != 0;
    cache_metadata(klass.context, std::move(cache_key), const_cast<Il2CppMethodInfo*>(match));
    return Result<MethodHandle>::success(std::move(result));
}

} /// namespace il2cpp_native_bridge::detail

namespace il2cpp_native_bridge::detail {

Result<Il2CppObject*> MethodQueryBase::invoke_value(Il2CppObject* instance, std::span<void*> arguments) const {
    if (!method_) return Result<Il2CppObject*>::failure(method_.error());
    if (!method_.value().is_static && instance == nullptr) {
        return Result<Il2CppObject*>::failure(make_error(ErrorCode::invalid_instance,
            "instance method requires a managed object"));
    }
    auto outcome = invoke_raw(method_.value(), instance, arguments);
    if (!outcome) return Result<Il2CppObject*>::failure(outcome.error());
    return Result<Il2CppObject*>::success(outcome.value().value);
}

Result<void> MethodQueryBase::copy_value(Il2CppObject* value, void* destination, std::size_t size) const {
    if (!method_) return Result<void>::failure(method_.error());
    if (value == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invocation_failed, "value-type invocation returned null"));
    }
    const auto api = api_for(method_.value().declaring_class.context);
    if (api.object_unbox == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::missing_api, "il2cpp_object_unbox is unavailable"));
    }
    void* unboxed = api.object_unbox(value);
    if (unboxed == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invocation_failed, "could not unbox method result"));
    }
    std::memcpy(destination, unboxed, size);
    return Result<void>::success();
}

Result<std::shared_ptr<HookState>> MethodQueryBase::install_value(std::shared_ptr<HookBackend> backend,
                                                                 void* replacement) const {
    if (!method_) return Result<std::shared_ptr<HookState>>::failure(method_.error());
    return install_hook(method_.value(), std::move(backend), replacement);
}

} /// namespace il2cpp_native_bridge::detail
