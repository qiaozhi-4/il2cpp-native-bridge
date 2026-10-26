#include "metadata_internal.hpp"

namespace il2cpp_native_bridge::detail {

Result<FieldHandle> resolve_field(const ClassHandle& klass,
                                  std::string_view name,
                                  std::string_view value_type) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<FieldHandle>::failure(valid.error());
    if (name.empty()) {
        return Result<FieldHandle>::failure(make_error(ErrorCode::invalid_argument, "field name is empty"));
    }
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<FieldHandle>::failure(attached.error());

    const auto api = api_for(klass.context);
    auto cache_key = metadata_cache_key("field", klass.generation, klass.native);
    append_metadata_cache_part(cache_key, name);
    append_metadata_cache_part(cache_key, value_type);
    if (auto* cached = find_cached_metadata(klass.context, cache_key); cached != nullptr) {
        auto* field = static_cast<const Il2CppFieldInfo*>(cached);
        auto actual_type = managed_name(api, api.field_get_type(field));
        if (!actual_type) return Result<FieldHandle>::failure(actual_type.error());
        FieldHandle result;
        result.declaring_class = klass;
        result.native = field;
        result.name = std::string(name);
        result.value_type = actual_type.take_value();
        result.is_static = (api.field_get_flags(field) & kFieldAttributeStatic) != 0;
        return Result<FieldHandle>::success(std::move(result));
    }
    const Il2CppFieldInfo* match = nullptr;
    std::size_t named_matches = 0;
    std::size_t exact_matches = 0;
    std::string matched_type;
    void* iterator = nullptr;
    while (const auto* field = api.class_get_fields(klass.native, &iterator)) {
        const char* field_name = api.field_get_name(field);
        if (field_name == nullptr || name != field_name) continue;
        ++named_matches;
        const auto actual_type = managed_name(api, api.field_get_type(field));
        if (!actual_type) return Result<FieldHandle>::failure(actual_type.error());
        if (!value_type.empty() && actual_type.value() != value_type) continue;
        match = field;
        matched_type = actual_type.value();
        ++exact_matches;
    }

    if (exact_matches == 0) {
        const auto code = named_matches == 0 ? ErrorCode::not_found : ErrorCode::type_mismatch;
        return Result<FieldHandle>::failure(make_error(code,
            named_matches == 0 ? "managed field was not found: " + std::string(name)
                               : "managed field type did not match: " + std::string(name)));
    }
    if (exact_matches > 1) {
        return Result<FieldHandle>::failure(make_error(ErrorCode::ambiguous_match,
            "managed field name and type matched multiple fields: " + std::string(name)));
    }

    FieldHandle result;
    result.declaring_class = klass;
    result.native = match;
    result.name = std::string(name);
    result.value_type = std::move(matched_type);
    result.is_static = (api.field_get_flags(match) & kFieldAttributeStatic) != 0;
    cache_metadata(klass.context, std::move(cache_key), const_cast<Il2CppFieldInfo*>(match));
    return Result<FieldHandle>::success(std::move(result));
}

Result<std::vector<FieldHandle>> resolve_fields(const ClassHandle& klass) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<std::vector<FieldHandle>>::failure(valid.error());
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<std::vector<FieldHandle>>::failure(attached.error());

    const auto api = api_for(klass.context);
    std::vector<FieldHandle> fields;
    void* iterator = nullptr;
    while (const auto* field = api.class_get_fields(klass.native, &iterator)) {
        const char* name = api.field_get_name(field);
        if (name == nullptr) continue;
        auto type_name = managed_name(api, api.field_get_type(field));
        if (!type_name) return Result<std::vector<FieldHandle>>::failure(type_name.error());

        FieldHandle handle;
        handle.declaring_class = klass;
        handle.native = field;
        handle.name = name;
        handle.value_type = type_name.take_value();
        handle.is_static = (api.field_get_flags(field) & kFieldAttributeStatic) != 0;
        fields.push_back(std::move(handle));
    }
    return Result<std::vector<FieldHandle>>::success(std::move(fields));
}

Result<void> read_field_raw(const FieldHandle& field, Il2CppObject* instance, void* destination, std::size_t size) {
    auto valid = validate_field_handle(field);
    if (!valid) return valid;
    if (destination == nullptr || size == 0) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "field destination is invalid"));
    }
    if (!field.is_static && instance == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_instance, "instance field requires a managed object"));
    }
    auto attached = acquire_thread(field.declaring_class.context);
    if (!attached) return Result<void>::failure(attached.error());
    const auto api = api_for(field.declaring_class.context);
    if (field.is_static) api.field_static_get_value(field.native, destination);
    else api.field_get_value(instance, field.native, destination);
    return Result<void>::success();
}

Result<void> write_field_raw(const FieldHandle& field, Il2CppObject* instance, void* source, std::size_t size) {
    auto valid = validate_field_handle(field);
    if (!valid) return valid;
    if (source == nullptr || size == 0) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "field source is invalid"));
    }
    if (!field.is_static && instance == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_instance, "instance field requires a managed object"));
    }
    auto attached = acquire_thread(field.declaring_class.context);
    if (!attached) return Result<void>::failure(attached.error());
    const auto api = api_for(field.declaring_class.context);
    if (field.is_static) api.field_static_set_value(field.native, source);
    else api.field_set_value(instance, field.native, source);
    return Result<void>::success();
}

Result<void> write_boxed_field(const FieldHandle& field, Il2CppObject* boxed_value, Il2CppObject* instance) {
    auto valid = validate_field_handle(field);
    if (!valid) return valid;
    if (boxed_value == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_argument, "boxed field value is null"));
    }
    if (!field.is_static && instance == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::invalid_instance, "instance field requires a managed object"));
    }
    auto attached = acquire_thread(field.declaring_class.context);
    if (!attached) return Result<void>::failure(attached.error());
    const auto api = api_for(field.declaring_class.context);
    if (api.object_unbox == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::missing_api, "il2cpp_object_unbox is unavailable"));
    }
    void* value = api.object_unbox(boxed_value);
    if (value == nullptr) {
        return Result<void>::failure(make_error(ErrorCode::type_mismatch, "managed value could not be unboxed"));
    }
    if (field.is_static) api.field_static_set_value(field.native, value);
    else api.field_set_value(instance, field.native, value);
    return Result<void>::success();
}

} /// namespace il2cpp_native_bridge::detail

namespace il2cpp_native_bridge::detail {

Result<void> FieldQueryBase::read_value(Il2CppObject* instance, void* destination, std::size_t size) const {
    if (!field_) return Result<void>::failure(field_.error());
    return read_field_raw(field_.value(), instance, destination, size);
}

Result<void> FieldQueryBase::write_value(Il2CppObject* instance, const void* source, std::size_t size) const {
    if (!field_) return Result<void>::failure(field_.error());
    /// IL2CPP 的写入导出沿用非 const 源指针，这里只是转发调用方的只读缓冲。
    return write_field_raw(field_.value(), instance, const_cast<void*>(source), size);
}

Result<void> FieldQueryBase::write_boxed_value(Il2CppObject* boxed_value, Il2CppObject* instance) const {
    if (!field_) return Result<void>::failure(field_.error());
    return write_boxed_field(field_.value(), boxed_value, instance);
}

} /// namespace il2cpp_native_bridge::detail
