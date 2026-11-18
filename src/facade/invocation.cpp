#include "il2cpp_native_bridge/il2cpp.hpp"

#include "facade/facade_internal.hpp"
#include "metadata/metadata_internal.hpp"

#include "logging/internal_log.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace INB::detail {
namespace {

/// 目标类型是否为值类型；缺少必要识别导出时明确失败，不猜类别。
bool is_value_type(const Api& api, const Il2CppType* type, std::string_view path) {
    if (api.class_from_type == nullptr || api.class_is_valuetype == nullptr) {
        fail(ErrorCode::missing_api, ErrorStage::bind, std::string(path),
             "type classification exports are unavailable");
    }
    Il2CppClass* klass = api.class_from_type(type);
    if (klass == nullptr) {
        fail(ErrorCode::missing_api, ErrorStage::bind, std::string(path),
             "il2cpp_class_from_type returned null");
    }
    return api.class_is_valuetype(klass);
}

void require_not_generic(const Api& api, const Il2CppMethodInfo* method, std::string_view path) {
    if (api.method_is_generic == nullptr || api.method_is_inflated == nullptr) {
        fail(ErrorCode::missing_api, ErrorStage::bind, std::string(path),
             "generic method identification exports are unavailable");
    }
    if (api.method_is_generic(method) || api.method_is_inflated(method)) {
        fail(ErrorCode::unsupported_runtime, ErrorStage::bind, std::string(path),
             "generic or inflated methods are not supported");
    }
}

void require_assignability_exports(const Api& api, std::string_view path) {
    if (api.object_get_class == nullptr || api.class_is_assignable_from == nullptr ||
        api.class_from_type == nullptr) {
        fail(ErrorCode::missing_api, ErrorStage::invoke, std::string(path),
             "reference validation exports are unavailable");
    }
}

} /// namespace

BoundSignature bind_method(const std::shared_ptr<RuntimeContext>& context,
                           std::uint64_t generation,
                           const std::vector<MethodDescriptor>& candidates,
                           std::string path,
                           bool return_is_reference,
                           bool return_is_string,
                           std::string_view return_name,
                           std::span<const ArgumentSpec> arguments,
                           bool allow_byref) {
    const auto& view = require_scope_for(context, generation, path);
    if (candidates.size() != 1) {
        fail(ErrorCode::ambiguous_match, ErrorStage::bind, std::move(path),
             "the method must be unique before it can be bound");
    }

    const auto& descriptor = candidates.front();
    require_not_generic(view.api, descriptor.native, path);

    if (descriptor.parameter_types.size() != arguments.size()) {
        fail(ErrorCode::invalid_argument, ErrorStage::bind, path,
             "the bound argument count does not match the method signature");
    }

    /// 返回类型：void 只允许 void；引用载体要求目标不是值类型；基础类型要求名称精确一致。
    if (descriptor.return_type.name == "System.Void") {
        if (return_is_reference || return_name != "System.Void") {
            fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                 "a void method can only be bound to a void result");
        }
    } else if (return_is_reference) {
        if (return_is_string) {
            if (descriptor.return_type.name != "System.String") {
                fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                     "the result is not a string");
            }
        } else if (is_value_type(view.api, descriptor.return_type.native, path)) {
            fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                 "a value-type result cannot be bound to a reference carrier");
        }
    } else if (descriptor.return_type.name != return_name) {
        fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
             "the result type does not match the bound C++ type");
    }

    std::vector<bool> reference_slots;
    reference_slots.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& specification = arguments[index];
        const auto* native = descriptor.parameter_types[index].native;
        const auto name = descriptor.parameter_types[index].name;

        /// byref/ref/out：托管类型名以 & 结尾。invoke 路径拒绝；replacement 路径在用 C++ 指针
        /// 载体（如 bool* 对应 System.Boolean&）精确声明时才接受，因为 native ABI 本就按地址透传。
        if (name.ends_with('&')) {
            const auto element_name = name.substr(0, name.size() - 1);
            if (!allow_byref || !specification.reference_carrier || specification.string_carrier ||
                element_name != specification.managed_name) {
                fail(ErrorCode::unsupported_runtime, ErrorStage::bind, path,
                     "byref, ref and out parameters are not supported");
            }
            /// byref 传的是值地址，不参与托管对象引用校验。
            reference_slots.push_back(false);
            continue;
        }

        reference_slots.push_back(specification.reference_carrier);
        if (specification.reference_carrier) {
            if (specification.string_carrier) {
                if (name != "System.String") {
                    fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                         "the parameter is not a string");
                }
            } else if (is_value_type(view.api, native, path)) {
                fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                     "a value-type parameter cannot be bound to a reference carrier");
            }
        } else if (name != specification.managed_name) {
            fail(ErrorCode::type_mismatch, ErrorStage::bind, path,
                 "a parameter type does not match the bound C++ type");
        }
    }

    BoundSignature binding;
    binding.context = context;
    binding.generation = generation;
    binding.descriptor = descriptor;
    binding.path = std::move(path);
    binding.reference_slots = std::move(reference_slots);
    if constexpr (INB_LOG_ENABLED) {
        if (mark_lookup_logged(context, binding.path)) INB_LOGD("bind %s", binding.path.c_str());
    }
    return binding;
}

Il2CppObject* invoke_bound(const BoundSignature& binding,
                           Il2CppObject* instance,
                           std::span<void*> arguments) {
    const auto& view = require_scope_for(binding.context, binding.generation, binding.path);
    const auto& descriptor = binding.descriptor;

    if (descriptor.is_static) {
        if (instance != nullptr) {
            fail(ErrorCode::invalid_instance, ErrorStage::invoke, binding.path,
                 "a static method does not take an instance");
        }
    } else {
        if (instance == nullptr) {
            fail(ErrorCode::invalid_instance, ErrorStage::invoke, binding.path,
                 "an instance method requires a managed object");
        }
        require_assignability_exports(view.api, binding.path);
        Il2CppClass* actual = view.api.object_get_class(instance);
        if (actual == nullptr) {
            fail(ErrorCode::invalid_instance, ErrorStage::invoke, binding.path,
                 "the instance has no managed class");
        }
        if (!view.api.class_is_assignable_from(descriptor.declaring_class, actual)) {
            fail(ErrorCode::invalid_instance, ErrorStage::invoke, binding.path,
                 "the instance is not compatible with the declaring class");
        }
    }

    /// 非空引用实参核对赋值兼容性；null 合法。基础类型槽在绑定期已按名称核对，
    /// 这里不再要求类型识别导出——否则缺 class_from_type 会连带阻断 primitive 调用。
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!binding.reference_slots[index]) continue;

        const auto* target = descriptor.parameter_types[index].native;
        auto* value = static_cast<Il2CppObject*>(arguments[index]);
        if (value == nullptr) continue;

        require_assignability_exports(view.api, binding.path);
        Il2CppClass* target_class = view.api.class_from_type(target);
        Il2CppClass* actual = view.api.object_get_class(value);
        if (target_class == nullptr || actual == nullptr) {
            fail(ErrorCode::missing_api, ErrorStage::invoke, binding.path,
                 "the argument class could not be identified");
        }
        if (!view.api.class_is_assignable_from(target_class, actual)) {
            fail(ErrorCode::type_mismatch, ErrorStage::invoke, binding.path,
                 "a reference argument is not assignable to the parameter type");
        }
    }

    Il2CppObject* exception = nullptr;
    Il2CppObject* result = view.api.runtime_invoke(descriptor.native, instance, arguments.data(), &exception);
    if (exception != nullptr) {
        fail(ErrorCode::managed_exception, ErrorStage::invoke, binding.path,
             "the managed method threw an exception");
    }
    return result;
}

void unbox_result(const BoundSignature& binding, Il2CppObject* boxed, void* destination, std::size_t size) {
    const auto& view = require_scope_for(binding.context, binding.generation, binding.path);
    /// 能力缺失优先报告：此时无论结果是否为空都无法安全取值。
    if (view.api.object_unbox == nullptr) {
        fail(ErrorCode::missing_api, ErrorStage::invoke, binding.path,
             "il2cpp_object_unbox is unavailable");
    }
    if (boxed == nullptr) {
        fail(ErrorCode::invocation_failed, ErrorStage::invoke, binding.path,
             "a value-type result was not produced");
    }
    void* data = view.api.object_unbox(boxed);
    if (data == nullptr) {
        fail(ErrorCode::invocation_failed, ErrorStage::invoke, binding.path,
             "the boxed result is empty");
    }
    std::memcpy(destination, data, size);
}

} /// namespace INB::detail
