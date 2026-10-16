#include "metadata_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

#include <memory>

namespace il2cpp_native_bridge::detail {

Result<void> validate_class_handle(const ClassHandle& klass) {
    if (klass.native == nullptr || klass.context == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "class handle is empty"));
    }
    auto ready = require_ready(klass.context);
    if (!ready) return ready;
    if (klass.generation != generation_for(klass.context)) {
        return Result<void>::failure(make_error(ErrorCode::stale_handle, "class handle belongs to a previous runtime generation"));
    }
    return Result<void>::success();
}

Result<void> validate_method_handle(const MethodHandle& method) {
    if (method.native == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "method handle is empty"));
    }
    return validate_class_handle(method.declaring_class);
}

Result<void> validate_field_handle(const FieldHandle& field) {
    if (field.native == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "field handle is empty"));
    }
    return validate_class_handle(field.declaring_class);
}

Result<std::string> managed_name(const Api& api, const Il2CppType* type) {
    if (api.type_get_name == nullptr || type == nullptr) {
        return Result<std::string>::failure(make_error(ErrorCode::missing_api, "managed type name is unavailable"));
    }
    char* raw_name = api.type_get_name(type);
    if (raw_name == nullptr) {
        return Result<std::string>::failure(make_error(ErrorCode::type_mismatch, "IL2CPP returned an empty type name"));
    }
    std::string name(raw_name);
    if (api.free_memory != nullptr) api.free_memory(raw_name);
    if (name.starts_with("class ")) name.erase(0, 6);
    if (name.starts_with("valuetype ")) name.erase(0, 10);
    if (name == "void") name = "System.Void";
    return Result<std::string>::success(std::move(name));
}

} /// namespace il2cpp_native_bridge::detail

namespace il2cpp_native_bridge {
namespace {

std::shared_ptr<detail::RuntimeContext> current_runtime_context() {
    static const auto context = std::make_shared<detail::RuntimeContext>();
    return context;
}

} /// namespace

Runtime Runtime::current() {
    const auto context = current_runtime_context();
    (void) detail::refresh_context(context);
    return Runtime(context);
}

Result<void> Runtime::set_symbol_lookup(void* module, RuntimeSymbolLookup lookup) {
    if (module == nullptr || lookup == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "module and IL2CPP symbol lookup are required"));
    }

    const auto context = current_runtime_context();
    void* previous_owned_module = nullptr;
    {
        std::lock_guard lock(context->mutex);
        if (context->snapshot.state == RuntimeState::ready || context->snapshot.generation != 0) {
            return Result<void>::failure(make_error(ErrorCode::invalid_argument,
                "IL2CPP symbol lookup must be configured before the runtime becomes ready"));
        }
        if (context->owns_module && context->module != nullptr && context->module != module) {
            previous_owned_module = context->module;
        }
        context->module = module;
        context->owns_module = false;
        context->symbol_lookup = lookup;
        context->api = {};
        context->domain = nullptr;
        context->snapshot.state = RuntimeState::initializing;
        context->snapshot.last_error.clear();
        context->snapshot.missing_apis.clear();
    }
    if (previous_owned_module != nullptr) detail::close_runtime_module(previous_owned_module);
    return Result<void>::success();
}

Result<void> Runtime::set_export_override(std::string_view symbol_name, void* address) {
    if (!symbol_name.starts_with("il2cpp_") || address == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "an IL2CPP symbol name and non-null export address are required"));
    }

    const auto context = current_runtime_context();
    std::lock_guard lock(context->mutex);
    if (context->snapshot.state == RuntimeState::ready || context->snapshot.generation != 0) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "IL2CPP export overrides must be registered before the runtime becomes ready"));
    }
    context->export_overrides.insert_or_assign(std::string(symbol_name), address);
    return Result<void>::success();
}

Runtime Runtime::for_testing(detail::Api api, Il2CppDomain* domain) {
    auto context = std::make_shared<detail::RuntimeContext>();
    context->api = api;
    context->domain = domain;
    context->injected_api = true;
    context->snapshot.generation = domain == nullptr ? 0 : 1;
    context->snapshot.state = domain == nullptr ? RuntimeState::initializing : RuntimeState::ready;
    return Runtime(std::move(context));
}

RuntimeSnapshot Runtime::snapshot() const {
    (void) detail::refresh_context(context_);
    if (context_ == nullptr) return {};
    std::lock_guard lock(context_->mutex);
    return context_->snapshot;
}

Result<void> Runtime::refresh() const {
    return detail::refresh_context(context_);
}

Result<void> Runtime::with_attached_thread(const std::function<Result<void>()>& action) const {
    if (!action) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument,
            "runtime action must not be empty"));
    }

    auto attachment = detail::acquire_thread(context_);
    if (!attachment) return Result<void>::failure(attachment.error());
    return action();
}

Result<std::vector<ClassQuery>> Runtime::classes(std::string_view name_filter) const {
    auto resolved = detail::resolve_classes(context_, name_filter);
    if (!resolved) return Result<std::vector<ClassQuery>>::failure(resolved.error());

    std::vector<ClassQuery> result;
    result.reserve(resolved.value().size());
    for (auto& handle : resolved.value()) {
        result.push_back(ClassQuery(*this, Result<ClassHandle>::success(std::move(handle))));
    }
    return Result<std::vector<ClassQuery>>::success(std::move(result));
}

ClassQuery Runtime::klass(std::string_view fully_qualified_name) const {
    return ClassQuery(*this, std::string(fully_qualified_name), std::nullopt);
}

ClassQuery Runtime::klass_of(Il2CppObject* instance) const {
    return ClassQuery(*this, detail::resolve_object_class(context_, instance));
}

ClassQuery Runtime::generic_class(std::string_view generic_type_name,
                                  std::span<const std::string_view> type_arguments,
                                  std::optional<std::string_view> assembly_name) const {
    std::optional<std::string> assembly;
    if (assembly_name.has_value()) assembly = std::string(*assembly_name);
    return ClassQuery(*this, detail::resolve_generic_class(context_, generic_type_name,
                                                            type_arguments, assembly));
}

Result<ClassHandle> Runtime::class_by_name(std::string_view fully_qualified_name,
                                           std::optional<std::string> assembly_name) const {
    return detail::resolve_class(context_, fully_qualified_name, assembly_name);
}

ClassQuery::ClassQuery(Runtime runtime, std::string full_name, std::optional<std::string> assembly_name)
    : runtime_(std::move(runtime)), full_name_(std::move(full_name)), assembly_name_(std::move(assembly_name)),
      result_(detail::resolve_class(runtime_.context_, full_name_, assembly_name_)) {}

ClassQuery::ClassQuery(Runtime runtime, Result<ClassHandle> result)
    : runtime_(std::move(runtime)), result_(std::move(result)) {
    if (result_) full_name_ = result_.value().full_name;
}

ClassQuery ClassQuery::in_assembly(std::string_view assembly_name) const {
    return ClassQuery(runtime_, full_name_, std::string(assembly_name));
}

FieldQuery<Il2CppObject*> ClassQuery::object_field(std::string_view name) const {
    if (!result_) return FieldQuery<Il2CppObject*>(Result<FieldHandle>::failure(result_.error()));
    return FieldQuery<Il2CppObject*>(detail::resolve_field(result_.value(), name, {}));
}

FieldQuery<Il2CppObject*> ClassQuery::boxed_field(std::string_view name) const {
    if (!result_) return FieldQuery<Il2CppObject*>(Result<FieldHandle>::failure(result_.error()));
    return FieldQuery<Il2CppObject*>(detail::resolve_field(result_.value(), name, {}));
}

Result<std::vector<FieldHandle>> ClassQuery::fields() const {
    if (!result_) return Result<std::vector<FieldHandle>>::failure(result_.error());
    return detail::resolve_fields(result_.value());
}

Result<std::vector<MethodMetadata>> ClassQuery::methods() const {
    if (!result_) return Result<std::vector<MethodMetadata>>::failure(result_.error());
    return detail::resolve_methods(result_.value());
}

Result<Il2CppObject*> ClassQuery::new_object() const {
    if (!result_) return Result<Il2CppObject*>::failure(result_.error());
    return detail::allocate_object(result_.value());
}

} /// namespace il2cpp_native_bridge
