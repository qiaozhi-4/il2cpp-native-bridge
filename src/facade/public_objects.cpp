#include "il2cpp_native_bridge/il2cpp.hpp"

#include "facade/facade_internal.hpp"
#include "metadata/metadata_internal.hpp"
#include "runtime/il2cpp_api.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace INB {
namespace detail {
namespace {

template <typename T>
void* opaque(T* p) { return const_cast<void*>(static_cast<const void*>(p)); }


ClassHandle class_handle_for(const std::shared_ptr<RuntimeContext>& context,
                             std::uint64_t generation,
                             Il2CppClass* native,
                             std::string full_name,
                             std::string assembly_name) {
    ClassHandle handle;
    handle.context = context;
    handle.native = native;
    handle.generation = generation;
    handle.full_name = std::move(full_name);
    handle.assembly_name = std::move(assembly_name);
    return handle;
}

void throw_result(const Result<void>& result, std::string_view path) {
    if (!result) {
        report_and_throw(result.error().code, result.error().stage, std::string(path), result.error().message);
    }
}

void* find_image(const ReadyView& view, std::string_view image_name) {
    std::size_t count = 0;
    const auto* assemblies = view.api.domain_get_assemblies(view.domain, &count);
    if (assemblies == nullptr) return nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* image = assemblies[i] == nullptr ? nullptr : view.api.assembly_get_image(assemblies[i]);
        if (image == nullptr) continue;
        const char* name = view.api.image_get_name(image);
        if (name != nullptr && image_name == name) return const_cast<void*>(static_cast<const void*>(image));
    }
    return nullptr;
}

MethodDescriptor descriptor_for(const ReadyView& view, const Il2CppMethodInfo* method, Il2CppClass* declaring_class) {
    MethodDescriptor descriptor;
    descriptor.native = detail::opaque(method);
    descriptor.declaring_class = declaring_class;
    const char* method_name = view.api.method_get_name(method);
    descriptor.name = method_name == nullptr ? std::string{} : std::string(method_name);

    auto return_type = managed_name(view.api, view.api.method_get_return_type(method));
    if (return_type) { descriptor.return_type.native = detail::opaque(view.api.method_get_return_type(method)); descriptor.return_type.name = return_type.take_value(); }

    const std::uint32_t count = view.api.method_get_param_count(method);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* type = view.api.method_get_param(method, i);
        auto name = managed_name(view.api, type);
        if (name) { TypeDescriptor item; item.native = detail::opaque(type); item.name = name.take_value(); descriptor.parameter_types.push_back(std::move(item)); }
    }
    std::uint32_t flags = 0;
    view.api.method_get_flags(method, &flags);
    descriptor.is_static = (flags & kMethodAttributeStatic) != 0;
    return descriptor;
}

} /// namespace
} /// namespace detail

Class Il2Cpp::klass(std::string_view full_name) {
    const auto& view = detail::require_access_scope();
    auto resolved = detail::resolve_class(view.context, full_name, std::nullopt);
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage,
                                 std::string(full_name), resolved.error().message);
    }
    const auto& handle = resolved.value();
    return Class(view.context, view.generation, handle.native, handle.full_name,
                 {}, handle.assembly_name);
}

Image Class::image() const {
    const auto& view = detail::require_scope_for(context_, generation_, full_name_ + "::image");
    const auto* native = detail::find_image(view, image_name_);
    if (native == nullptr) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, full_name_,
                                 "the image of this class is not loaded");
    }
    return Image(context_, generation_, detail::opaque(native), image_name_, assembly_name_);
}

std::vector<Field> Class::fields() const {
    auto handle = detail::class_handle_for(context_, generation_, static_cast<Il2CppClass*>(native_), full_name_, assembly_name_);
    auto resolved = detail::resolve_fields(handle);
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage, full_name_, resolved.error().message);
    }
    std::vector<Field> result;
    result.reserve(resolved.value().size());
    for (auto& field : resolved.value()) {
        result.push_back(Field(context_, generation_, detail::opaque(field.native), native_, nullptr,
                               field.name, field.value_type, field.is_static));
    }
    return result;
}

std::vector<Method> Class::methods() const {
    const auto& view = detail::require_scope_for(context_, generation_, full_name_ + "::methods");
    std::vector<Method> result;
    void* iterator = nullptr;
    while (const auto* method = view.api.class_get_methods(static_cast<Il2CppClass*>(native_), &iterator)) {
        auto descriptor = detail::descriptor_for(view, method, static_cast<Il2CppClass*>(native_));
        std::vector<MethodDescriptor> candidates{descriptor};
        result.push_back(Method(context_, generation_, native_, full_name_, descriptor.name,
                                std::move(candidates)));
    }
    return result;
}

Field Class::field(std::string_view name) const {
    auto handle = detail::class_handle_for(context_, generation_, static_cast<Il2CppClass*>(native_), full_name_, assembly_name_);
    auto resolved = detail::resolve_field(handle, name, {});
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage,
                                 full_name_ + "::" + std::string(name), resolved.error().message);
    }
    const auto& field = resolved.value();
    return Field(context_, generation_, detail::opaque(field.native), native_, nullptr,
                 field.name, field.value_type, field.is_static);
}

Object Class::newObject() const {
    auto handle = detail::class_handle_for(context_, generation_, static_cast<Il2CppClass*>(native_), full_name_, assembly_name_);
    auto instance = detail::allocate_object(handle);
    if (!instance) {
        detail::report_and_throw(instance.error().code, instance.error().stage, full_name_,
                                 instance.error().message);
    }
    return Object(context_, generation_, instance.take_value(), native_, full_name_);
}

std::string Class::toString() const { return full_name_; }

Field::Field(std::shared_ptr<detail::RuntimeContext> context,
             std::uint64_t generation,
             void* native,
             void* declaring_class,
             void* instance,
             std::string name,
             std::string value_type,
             bool is_static)
    : context_(std::move(context)), generation_(generation), native_(native),
      declaring_class_(declaring_class), instance_(instance), name_(std::move(name)),
      value_type_(std::move(value_type)), is_static_(is_static) {}

Object Field::getObject() const {
    void* raw = nullptr;
    detail::field_read(context_, generation_, native_, declaring_class_, instance_, &raw, sizeof(raw), is_static_);
    return Object(context_, generation_, static_cast<Il2CppObject*>(raw), declaring_class_, {});
}

void Field::set_boxed(Object value) const {
    detail::field_write_boxed(context_, generation_, native_, declaring_class_, instance_,
                              static_cast<Il2CppObject*>(value.native_handle()), is_static_);
}

std::string Field::toString() const { return name_; }

Object::Object(std::shared_ptr<detail::RuntimeContext> context,
               std::uint64_t generation,
               void* native,
               void* declaring_class,
               std::string class_name)
    : context_(std::move(context)), generation_(generation), native_(native),
      declaring_class_(declaring_class), class_name_(std::move(class_name)) {}

Class Object::klass() const {
    auto resolved = detail::resolve_object_class(context_, static_cast<Il2CppObject*>(native_));
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage, class_name_,
                                 resolved.error().message);
    }
    const auto& handle = resolved.value();
    return Class(context_, generation_, handle.native, handle.full_name, {}, handle.assembly_name);
}

Field Object::field(std::string_view name) const {
    auto handle = detail::resolve_object_class(context_, static_cast<Il2CppObject*>(native_));
    if (!handle) {
        detail::report_and_throw(handle.error().code, handle.error().stage, class_name_, handle.error().message);
    }
    auto resolved = detail::resolve_field(handle.value(), name, {});
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage,
                                 class_name_ + "::" + std::string(name), resolved.error().message);
    }
    const auto& field = resolved.value();
    return Field(context_, generation_, detail::opaque(field.native), field.declaring_class.native, native_,
                 field.name, field.value_type, field.is_static);
}

Method Object::method(std::string_view name) const {
    return method(name, std::numeric_limits<std::size_t>::max());
}

Method Object::method(std::string_view name, std::size_t managed_parameter_count) const {
    auto handle = detail::resolve_object_class(context_, static_cast<Il2CppObject*>(native_));
    if (!handle) {
        detail::report_and_throw(handle.error().code, handle.error().stage, class_name_, handle.error().message);
    }
    if (managed_parameter_count == std::numeric_limits<std::size_t>::max()) {
        /// 数量未指定时按名字枚举，多个候选交给 overload 收窄。
        std::vector<MethodDescriptor> candidates;
        const auto& view = detail::require_scope_for(context_, generation_, class_name_);
        void* iterator = nullptr;
        while (const auto* method = view.api.class_get_methods(handle.value().native, &iterator)) {
            auto descriptor = detail::descriptor_for(view, method, handle.value().native);
            if (descriptor.name == name) candidates.push_back(std::move(descriptor));
        }
        if (candidates.empty()) {
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, class_name_ + "::" + std::string(name),
                                     "the method was not found");
        }
        return Method(context_, generation_, handle.value().native, handle.value().full_name,
                      std::string(name), std::move(candidates), native_);
    }
    auto resolved = detail::resolve_method_by_arity(handle.value(), name, managed_parameter_count);
    if (!resolved) {
        detail::report_and_throw(resolved.error().code, resolved.error().stage,
                                 class_name_ + "::" + std::string(name), resolved.error().message);
    }
    const auto& view = detail::require_scope_for(context_, generation_, class_name_);
    auto descriptor = detail::descriptor_for(view, resolved.value().native, resolved.value().declaring_class.native);
    std::vector<MethodDescriptor> candidates{descriptor};
    return Method(context_, generation_, resolved.value().declaring_class.native,
                  resolved.value().declaring_class.full_name, std::string(name),
                  std::move(candidates), native_);
}

String Object::as_string() const { return String(context_, generation_, reinterpret_cast<Il2CppString*>(native_)); }

GcHandle Object::ref(bool pin) const {
    return GcHandle(context_, detail::gchandle_new(context_, generation_, native_, pin, false));
}

GcHandle Object::weak_ref(bool pin) const {
    return GcHandle(context_, detail::gchandle_new(context_, generation_, native_, pin, true));
}

std::string Object::toString() const { return class_name_; }

String::String(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* native)
    : context_(std::move(context)), generation_(generation), native_(native) {}

std::string String::content() const { return detail::string_content(context_, generation_, native_); }

Object String::object() const {
    return Object(context_, generation_, reinterpret_cast<Il2CppObject*>(native_), nullptr, "System.String");
}

Object GcHandle::target() const {
    auto* raw = detail::gchandle_target(context_, handle_);
    return Object(context_, 0, raw, nullptr, {});
}

void GcHandle::free() noexcept {
    if (handle_ == 0) return;
    try {
        detail::gchandle_free(context_, handle_);
    } catch (...) {
    }
    handle_ = 0;
}

namespace detail {

void field_read(const std::shared_ptr<RuntimeContext>& context,
                std::uint64_t generation,
                void* field,
                void* declaring_class,
                void* instance,
                void* destination,
                std::size_t size,
                bool is_static) {
    FieldHandle handle;
    handle.declaring_class = class_handle_for(context, generation, static_cast<Il2CppClass*>(declaring_class), {}, {});
    handle.native = static_cast<const Il2CppFieldInfo*>(field);
    handle.name = "<new-api>";
    handle.is_static = is_static;
    auto result = read_field_raw(handle, static_cast<Il2CppObject*>(instance), destination, size);
    throw_result(result, "field::get");
}

void field_write(const std::shared_ptr<RuntimeContext>& context,
                 std::uint64_t generation,
                 void* field,
                 void* declaring_class,
                 void* instance,
                 const void* source,
                 std::size_t size,
                 bool is_static) {
    FieldHandle handle;
    handle.declaring_class = class_handle_for(context, generation, static_cast<Il2CppClass*>(declaring_class), {}, {});
    handle.native = static_cast<const Il2CppFieldInfo*>(field);
    handle.name = "<new-api>";
    handle.is_static = is_static;
    auto result = write_field_raw(handle, static_cast<Il2CppObject*>(instance), const_cast<void*>(source), size);
    throw_result(result, "field::set");
}

void field_write_boxed(const std::shared_ptr<RuntimeContext>& context,
                       std::uint64_t generation,
                       void* field,
                       void* declaring_class,
                       void* instance,
                       void* boxed,
                       bool is_static) {
    FieldHandle handle;
    handle.declaring_class = class_handle_for(context, generation, static_cast<Il2CppClass*>(declaring_class), {}, {});
    handle.native = static_cast<const Il2CppFieldInfo*>(field);
    handle.name = "<new-api>";
    handle.is_static = is_static;
    auto result = write_boxed_field(handle, static_cast<Il2CppObject*>(boxed), static_cast<Il2CppObject*>(instance));
    throw_result(result, "field::set_boxed");
}

std::uint32_t gchandle_new(const std::shared_ptr<RuntimeContext>& context,
                           std::uint64_t generation,
                           void* target,
                           bool pinned,
                           bool weak) {
    const auto& view = require_scope_for(context, generation, "gchandle::new");
    if (target == nullptr) {
        report_and_throw(ErrorCode::invalid_argument, ErrorStage::invoke, "gchandle::new",
                         "a GCHandle target must not be null");
    }
    if (weak) {
        if (view.api.gchandle_new_weakref == nullptr) {
            report_and_throw(ErrorCode::missing_api, ErrorStage::invoke, "gchandle::new",
                             "il2cpp_gchandle_new_weakref is unavailable");
        }
        return view.api.gchandle_new_weakref(static_cast<Il2CppObject*>(target), pinned);
    }
    if (view.api.gchandle_new == nullptr) {
        report_and_throw(ErrorCode::missing_api, ErrorStage::invoke, "gchandle::new",
                         "il2cpp_gchandle_new is unavailable");
    }
    return view.api.gchandle_new(static_cast<Il2CppObject*>(target), pinned);
}

void gchandle_free(const std::shared_ptr<RuntimeContext>& context, std::uint32_t handle) {
    const auto& view = require_scope_for(context, 0, "gchandle::free");
    if (view.api.gchandle_free != nullptr) view.api.gchandle_free(handle);
}

void* gchandle_target(const std::shared_ptr<RuntimeContext>& context, std::uint32_t handle) {
    const auto& view = require_scope_for(context, 0, "gchandle::target");
    if (view.api.gchandle_get_target == nullptr) {
        report_and_throw(ErrorCode::missing_api, ErrorStage::invoke, "gchandle::target",
                         "il2cpp_gchandle_get_target is unavailable");
    }
    return view.api.gchandle_get_target(handle);
}

std::string string_content(const std::shared_ptr<RuntimeContext>& context,
                           std::uint64_t generation,
                           void* value) {
    (void) context;
    (void) generation;
    auto result = Runtime::current().string_to_utf8(static_cast<Il2CppString*>(value));
    if (!result) {
        report_and_throw(result.error().code, result.error().stage, "string::content", result.error().message);
    }
    return result.take_value();
}

std::size_t array_length(const std::shared_ptr<RuntimeContext>& context,
                         std::uint64_t generation,
                         void* array) {
    (void) context;
    (void) generation;
    auto result = Runtime::current().array_length(static_cast<Il2CppObject*>(array));
    if (!result) {
        report_and_throw(result.error().code, result.error().stage, "array::length", result.error().message);
    }
    return result.take_value();
}

void* array_element(const std::shared_ptr<RuntimeContext>& context,
                    std::uint64_t generation,
                    void* array,
                    std::size_t index) {
    (void) context;
    (void) generation;
    /// GetValue 在 System.Array 上有多个单参重载，必须按完整签名而不是参数个数选取。
    auto getter = Runtime::current().klass("System.Array")
            .method<Il2CppObject*, std::int32_t>("GetValue");
    auto value = getter.invoke_on(static_cast<Il2CppObject*>(array), static_cast<std::int32_t>(index));
    if (!value) {
        report_and_throw(value.error().code, value.error().stage, "array::at", value.error().message);
    }
    return value.take_value();
}

void* unbox_value(const std::shared_ptr<RuntimeContext>& context,
                  std::uint64_t generation,
                  void* boxed) {
    (void) context;
    (void) generation;
    if (boxed == nullptr) return nullptr;
    auto result = Runtime::current().unbox(static_cast<Il2CppObject*>(boxed));
    if (!result) {
        report_and_throw(result.error().code, result.error().stage, "value::unbox", result.error().message);
    }
    return result.take_value();
}

} /// namespace detail
} /// namespace INB
