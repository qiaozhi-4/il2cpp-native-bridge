#include "metadata_internal.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace INB::detail {

Result<ClassHandle> resolve_class(const std::shared_ptr<RuntimeContext>& context,
                                  std::string_view full_name,
                                  const std::optional<std::string>& assembly_name) {
    if (full_name.empty()) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invalid_argument, "class name is empty"));
    }
    auto ready = require_ready(context);
    if (!ready) return Result<ClassHandle>::failure(ready.error());
    auto attached = acquire_thread(context);
    if (!attached) return Result<ClassHandle>::failure(attached.error());

    const auto dot = full_name.rfind('.');
    const std::string name_space = dot == std::string_view::npos ? "" : std::string(full_name.substr(0, dot));
    const std::string class_name = dot == std::string_view::npos ? std::string(full_name) : std::string(full_name.substr(dot + 1));
    if (class_name.empty()) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invalid_argument, "class name is empty"));
    }

    /// 无论是否限定程序集都查缓存：无程序集限定时同样能命中，避免每次全量扫描。
    const auto generation = generation_for(context);
    auto cache_key = metadata_cache_key("class", generation, nullptr);
    append_metadata_cache_part(cache_key, full_name);
    if (assembly_name.has_value()) append_metadata_cache_part(cache_key, *assembly_name);

    ClassCacheEntry cached;
    if (find_cached_class(context, cache_key, cached)) {
        ClassHandle handle;
        handle.context = context;
        handle.native = cached.native;
        handle.generation = generation;
        handle.full_name = std::string(full_name);
        handle.assembly_name = std::move(cached.assembly_name);
        return Result<ClassHandle>::success(std::move(handle));
    }

    const auto api = api_for(context);
    Il2CppDomain* domain = nullptr;
    {
        std::lock_guard lock(context->mutex);
        domain = context->domain;
    }
    std::size_t assembly_count = 0;
    const auto** assemblies = api.domain_get_assemblies(domain, &assembly_count);
    if (assemblies == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::runtime_not_ready, "IL2CPP returned no assembly list"));
    }

    Il2CppClass* matched = nullptr;
    std::string matched_assembly;
    std::size_t matches = 0;
    for (std::size_t index = 0; index < assembly_count; ++index) {
        const auto* assembly = assemblies[index];
        const auto* image = assembly == nullptr ? nullptr : api.assembly_get_image(assembly);
        if (image == nullptr) continue;
        const char* image_name = api.image_get_name(image);
        const std::string current_assembly = image_name == nullptr ? "" : image_name;
        if (assembly_name.has_value() && !assembly_name_matches(current_assembly, *assembly_name)) continue;

        auto* klass = api.class_from_name(image, name_space.c_str(), class_name.c_str());
        if (klass == nullptr) continue;
        ++matches;
        matched = klass;
        matched_assembly = current_assembly;
    }

    if (matches == 0) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::not_found,
            "managed class was not found: " + std::string(full_name)));
    }
    if (matches > 1) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::ambiguous_match,
            "managed class matched multiple assemblies: " + std::string(full_name)));
    }

    cache_class(context, std::move(cache_key), matched, matched_assembly);

    ClassHandle found;
    found.context = context;
    found.native = matched;
    found.generation = generation;
    found.full_name = std::string(full_name);
    found.assembly_name = std::move(matched_assembly);
    return Result<ClassHandle>::success(std::move(found));
}

Result<std::vector<ClassHandle>> resolve_classes(const std::shared_ptr<RuntimeContext>& context,
                                                 std::string_view name_filter) {
    auto ready = require_ready(context);
    if (!ready) return Result<std::vector<ClassHandle>>::failure(ready.error());
    auto attached = acquire_thread(context);
    if (!attached) return Result<std::vector<ClassHandle>>::failure(attached.error());

    const auto api = api_for(context);
    if (api.image_get_class_count == nullptr || api.image_get_class == nullptr) {
        return Result<std::vector<ClassHandle>>::failure(make_error(ErrorCode::missing_api,
            "IL2CPP image class enumeration exports are unavailable"));
    }

    Il2CppDomain* domain = nullptr;
    {
        std::lock_guard lock(context->mutex);
        domain = context->domain;
    }
    std::size_t assembly_count = 0;
    const auto** assemblies = api.domain_get_assemblies(domain, &assembly_count);
    if (assemblies == nullptr) {
        return Result<std::vector<ClassHandle>>::failure(make_error(ErrorCode::runtime_not_ready,
            "IL2CPP returned no assembly list"));
    }

    /// 代际在整次枚举期间不变，循环外取一次即可，避免每个类都取一次锁。
    const auto generation = generation_for(context);

    std::vector<ClassHandle> classes;
    for (std::size_t assembly_index = 0; assembly_index < assembly_count; ++assembly_index) {
        const auto* assembly = assemblies[assembly_index];
        const auto* image = assembly == nullptr ? nullptr : api.assembly_get_image(assembly);
        if (image == nullptr) continue;

        const char* image_name = api.image_get_name(image);
        const std::string assembly_name = image_name == nullptr ? "" : image_name;
        const std::size_t class_count = api.image_get_class_count(image);
        for (std::size_t class_index = 0; class_index < class_count; ++class_index) {
            Il2CppClass* native = api.image_get_class(image, class_index);
            if (native == nullptr) continue;

            const char* class_name = api.class_get_name(native);
            if (class_name == nullptr || *class_name == '\0') continue;
            const char* name_space = api.class_get_namespace(native);
            std::string full_name = (name_space != nullptr && *name_space != '\0')
                ? std::string(name_space) + "." + class_name
                : std::string(class_name);
            if (!name_filter.empty() && full_name.find(name_filter) == std::string::npos) continue;

            ClassHandle handle;
            handle.context = context;
            handle.native = native;
            handle.generation = generation;
            handle.full_name = std::move(full_name);
            handle.assembly_name = assembly_name;
            classes.push_back(std::move(handle));
        }
    }

    std::sort(classes.begin(), classes.end(), [](const ClassHandle& left, const ClassHandle& right) {
        if (left.full_name != right.full_name) return left.full_name < right.full_name;
        return left.assembly_name < right.assembly_name;
    });
    return Result<std::vector<ClassHandle>>::success(std::move(classes));
}

Result<ClassHandle> resolve_generic_class(const std::shared_ptr<RuntimeContext>& context,
                                          std::string_view generic_type_name,
                                          std::span<const std::string_view> type_arguments,
                                          const std::optional<std::string>& assembly_name) {
    if (generic_type_name.empty() || type_arguments.empty()) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invalid_argument,
            "generic class name and type arguments must not be empty"));
    }
    for (const auto argument : type_arguments) {
        if (argument.empty()) {
            return Result<ClassHandle>::failure(make_error(ErrorCode::invalid_argument,
                "generic type argument name is empty"));
        }
    }

    auto generic_definition = resolve_class(context, generic_type_name, assembly_name);
    if (!generic_definition) return generic_definition;

    const auto api = api_for(context);
    if (api.class_get_type == nullptr || api.type_get_object == nullptr || api.array_new == nullptr ||
        api.class_from_system_type == nullptr || api.runtime_invoke == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::missing_api,
            "IL2CPP reflection APIs required for generic class inflation are unavailable"));
    }

    auto reflection_type = resolve_class(context, "System.Type", std::nullopt);
    if (!reflection_type) return Result<ClassHandle>::failure(reflection_type.error());
    auto array_class = resolve_class(context, "System.Array", std::nullopt);
    if (!array_class) return Result<ClassHandle>::failure(array_class.error());

    const auto* definition_type = api.class_get_type(generic_definition.value().native);
    Il2CppObject* definition_type_object = definition_type == nullptr ? nullptr : api.type_get_object(definition_type);
    if (definition_type_object == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invocation_failed,
            "could not create a reflection object for the generic type definition"));
    }

    Il2CppArray* argument_array = api.array_new(reflection_type.value().native, type_arguments.size());
    if (argument_array == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invocation_failed,
            "could not allocate the generic type argument array"));
    }
    auto set_value = resolve_method(array_class.value(), "SetValue", "System.Void",
                                    {"System.Object", "System.Int32"});
    if (!set_value) return Result<ClassHandle>::failure(set_value.error());

    std::string closed_name(generic_type_name);
    closed_name.push_back('<');
    for (std::size_t index = 0; index < type_arguments.size(); ++index) {
        auto argument_class = resolve_class(context, type_arguments[index], std::nullopt);
        if (!argument_class) return Result<ClassHandle>::failure(argument_class.error());
        const auto* argument_type = api.class_get_type(argument_class.value().native);
        Il2CppObject* argument_type_object = argument_type == nullptr ? nullptr : api.type_get_object(argument_type);
        if (argument_type_object == nullptr) {
            return Result<ClassHandle>::failure(make_error(ErrorCode::invocation_failed,
                "could not create a reflection object for generic type argument: " + std::string(type_arguments[index])));
        }

        std::int32_t array_index = static_cast<std::int32_t>(index);
        void* set_value_arguments[] = {&argument_type_object, &array_index};
        Il2CppObject* exception = nullptr;
        (void) api.runtime_invoke(set_value.value().native, reinterpret_cast<Il2CppObject*>(argument_array),
                                  set_value_arguments, &exception);
        if (exception != nullptr) {
            return Result<ClassHandle>::failure(make_error(ErrorCode::invocation_failed,
                "could not populate the generic type argument array"));
        }
        if (index != 0) closed_name.append(", ");
        closed_name.append(type_arguments[index]);
    }
    closed_name.push_back('>');

    auto concrete_type_class = resolve_object_class(context, definition_type_object);
    if (!concrete_type_class) return Result<ClassHandle>::failure(concrete_type_class.error());
    auto make_generic_type = resolve_method_by_arity(concrete_type_class.value(), "MakeGenericType", 1);
    if (!make_generic_type) {
        make_generic_type = resolve_method_by_arity(reflection_type.value(), "MakeGenericType", 1);
    }
    if (!make_generic_type) return Result<ClassHandle>::failure(make_generic_type.error());

    Il2CppObject* argument_array_object = reinterpret_cast<Il2CppObject*>(argument_array);
    void* make_generic_type_arguments[] = {&argument_array_object};
    Il2CppObject* exception = nullptr;
    Il2CppObject* closed_type_object = api.runtime_invoke(make_generic_type.value().native,
                                                           definition_type_object,
                                                           make_generic_type_arguments,
                                                           &exception);
    if (exception != nullptr || closed_type_object == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invocation_failed,
            "IL2CPP could not inflate generic class: " + closed_name));
    }

    Il2CppClass* closed_class = api.class_from_system_type(closed_type_object);
    if (closed_class == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::not_found,
            "IL2CPP returned no class for inflated generic type: " + closed_name));
    }

    ClassHandle result;
    result.context = context;
    result.native = closed_class;
    result.generation = generation_for(context);
    result.full_name = std::move(closed_name);
    result.assembly_name = generic_definition.value().assembly_name;
    return Result<ClassHandle>::success(std::move(result));
}

Result<ClassHandle> resolve_object_class(const std::shared_ptr<RuntimeContext>& context, Il2CppObject* instance) {
    if (instance == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::invalid_instance, "managed object is null"));
    }
    auto ready = require_ready(context);
    if (!ready) return Result<ClassHandle>::failure(ready.error());
    auto attached = acquire_thread(context);
    if (!attached) return Result<ClassHandle>::failure(attached.error());

    const auto api = api_for(context);
    if (api.object_get_class == nullptr || api.class_get_name == nullptr || api.class_get_namespace == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::missing_api, "managed object class metadata is unavailable"));
    }
    Il2CppClass* native = api.object_get_class(instance);
    if (native == nullptr) {
        return Result<ClassHandle>::failure(make_error(ErrorCode::not_found, "managed object has no class"));
    }
    const char* class_name = api.class_get_name(native);
    const char* name_space = api.class_get_namespace(native);
    if (class_name == nullptr || *class_name == '\0') {
        return Result<ClassHandle>::failure(make_error(ErrorCode::not_found, "managed object class has no name"));
    }

    ClassHandle result;
    result.context = context;
    result.native = native;
    result.generation = generation_for(context);
    result.full_name = (name_space != nullptr && *name_space != '\0')
        ? std::string(name_space) + "." + class_name
        : std::string(class_name);
    return Result<ClassHandle>::success(std::move(result));
}

Result<Il2CppObject*> allocate_object(const ClassHandle& klass) {
    auto valid = validate_class_handle(klass);
    if (!valid) return Result<Il2CppObject*>::failure(valid.error());
    auto attached = acquire_thread(klass.context);
    if (!attached) return Result<Il2CppObject*>::failure(attached.error());
    const auto api = api_for(klass.context);
    if (api.object_new == nullptr) {
        return Result<Il2CppObject*>::failure(make_error(ErrorCode::missing_api, "il2cpp_object_new is unavailable"));
    }
    Il2CppObject* instance = api.object_new(klass.native);
    if (instance == nullptr) {
        return Result<Il2CppObject*>::failure(make_error(ErrorCode::invocation_failed, "IL2CPP could not allocate the managed object"));
    }
    return Result<Il2CppObject*>::success(instance);
}

} /// namespace INB::detail
