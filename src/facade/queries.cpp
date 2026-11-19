#include "il2cpp_native_bridge/il2cpp.hpp"

#include "facade/facade_internal.hpp"
#include "metadata/metadata_internal.hpp"

#include "logging/internal_log.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace INB {
namespace {

using detail::ReadyView;

std::string join_path(std::initializer_list<std::string_view> parts) {
    std::string path;
    for (const auto part : parts) {
        if (part.empty()) continue;
        if (!path.empty()) path += '/';
        path += part;
    }
    return path;
}

/// 签名描述只服务于轨迹日志；日志关闭时该函数不参与编译。
[[maybe_unused]] std::string describe_type(const detail::Api& api, const Il2CppType* type, std::string_view path) {
    auto name = detail::managed_name(api, type);
    if (!name) {
        detail::report_and_throw(name.error().code, ErrorStage::lookup, std::string(path), name.error().message);
    }
    return std::move(name).take_value();
}

} /// namespace

Domain Il2Cpp::domain() {
    const auto& view = detail::require_access_scope();
    if (view.domain == nullptr) {
        detail::report_and_throw(ErrorCode::internal_error, ErrorStage::scope, {}, "the active scope has no IL2CPP domain");
    }
    return Domain(view.context, view.generation, view.domain);
}

Assembly Domain::assembly(std::string_view name) const {
    const auto requested = std::string(name);
    const auto& view = detail::require_scope_for(context_, generation_, requested);
    if (view.api.domain_get_assemblies == nullptr || view.api.assembly_get_image == nullptr ||
        view.api.image_get_name == nullptr) {
        detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, requested, "assembly enumeration exports are unavailable");
    }

    std::size_t count = 0;
    const Il2CppAssembly** assemblies = view.api.domain_get_assemblies(static_cast<Il2CppDomain*>(native_), &count);
    if (assemblies == nullptr || count == 0) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, requested, "the IL2CPP domain has no loaded assemblies");
    }

    const Il2CppAssembly* matched = nullptr;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* image = view.api.assembly_get_image(assemblies[index]);
        const char* image_name = image == nullptr ? nullptr : view.api.image_get_name(image);
        if (image_name == nullptr) continue;
        if (!detail::assembly_name_matches(image_name, requested)) continue;
        if (matched != nullptr) {
            detail::report_and_throw(ErrorCode::ambiguous_match, ErrorStage::lookup, requested,
                 "more than one loaded assembly matches the requested name");
        }
        matched = assemblies[index];
    }
    if (matched == nullptr) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, requested, "no loaded assembly matches the requested name");
    }
    return Assembly(context_, generation_, detail::opaque(matched), requested);
}

Image Assembly::image() const {
    const auto& view = detail::require_scope_for(context_, generation_, name_);
    const auto* image = view.api.assembly_get_image(static_cast<Il2CppAssembly*>(native_));
    if (image == nullptr) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, name_, "the assembly has no loaded image");
    }
    const char* image_name = view.api.image_get_name(image);
    return Image(context_, generation_, detail::opaque(image), image_name == nullptr ? name_ : std::string(image_name), name_);
}

Class Image::klass(std::string_view full_name) const {
    const auto path = join_path({assembly_name_, name_, full_name});
    const auto& view = detail::require_scope_for(context_, generation_, path);
    if (view.api.class_from_name == nullptr) {
        detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, path, "class lookup export is unavailable");
    }

    const auto separator = full_name.rfind('.');
    const std::string name_space = separator == std::string_view::npos
            ? std::string()
            : std::string(full_name.substr(0, separator));
    const std::string short_name = separator == std::string_view::npos
            ? std::string(full_name)
            : std::string(full_name.substr(separator + 1));

    Il2CppClass* klass = view.api.class_from_name(static_cast<Il2CppImage*>(native_), name_space.c_str(), short_name.c_str());
    if (klass == nullptr) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, path,
             "the selected image does not contain the requested class");
    }
    if constexpr (INB_LOG_ENABLED) {
        if (detail::mark_lookup_logged(context_, path)) {
            INB_LOGD("class %s -> %p", path.c_str(), static_cast<void*>(klass));
        }
    }
    return Class(context_, generation_, detail::opaque(klass), std::string(full_name), name_, assembly_name_);
}

Class::Class(std::shared_ptr<detail::RuntimeContext> context,
             std::uint64_t generation,
             void* native,
             std::string full_name,
             std::string image_name,
             std::string assembly_name)
    : context_(std::move(context)),
      generation_(generation),
      native_(native),
      full_name_(std::move(full_name)),
      image_name_(std::move(image_name)),
      assembly_name_(std::move(assembly_name)) {}

Method Class::method(std::string_view name) const { return select(name, std::nullopt); }

Method Class::method(std::string_view name, std::size_t managed_parameter_count) const {
    return select(name, managed_parameter_count);
}

Method Class::select(std::string_view name, const std::optional<std::size_t>& parameter_count) const {
    const auto path = join_path({assembly_name_, image_name_, full_name_}) + "::" + std::string(name);
    const auto& view = detail::require_scope_for(context_, generation_, path);
    if (view.api.class_get_methods == nullptr || view.api.method_get_name == nullptr ||
        view.api.method_get_param_count == nullptr || view.api.method_get_param == nullptr ||
        view.api.method_get_return_type == nullptr || view.api.method_get_flags == nullptr) {
        detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, path, "method enumeration exports are unavailable");
    }

    std::vector<MethodDescriptor> candidates;

    /// il2cpp_class_get_methods 只返回本类声明的方法；单例 getter 常在基类，沿继承链继续找。
    auto collect = [&](Il2CppClass* klass) {
        void* iterator = nullptr;
        while (const Il2CppMethodInfo* method = view.api.class_get_methods(klass, &iterator)) {
            const char* method_name = view.api.method_get_name(method);
            if (method_name == nullptr || name != method_name) continue;

            const std::uint32_t native_parameter_count = view.api.method_get_param_count(method);
            if (parameter_count.has_value() && parameter_count.value() != native_parameter_count) continue;

            MethodDescriptor descriptor = detail::descriptor_for(view, method, klass, path);
            descriptor.declaring_class_name = full_name_;
            candidates.push_back(std::move(descriptor));
        }
    };

    collect(static_cast<Il2CppClass*>(native_));
    if (candidates.empty() && view.api.class_get_parent != nullptr) {
        Il2CppClass* parent = view.api.class_get_parent(static_cast<Il2CppClass*>(native_));
        while (parent != nullptr) {
            collect(parent);
            if (!candidates.empty()) break;
            parent = view.api.class_get_parent(parent);
        }
    }

    if (candidates.empty()) {
        if constexpr (INB_LOG_ENABLED) {
            void* iterator = nullptr;
            while (const Il2CppMethodInfo* method = view.api.class_get_methods(static_cast<Il2CppClass*>(native_), &iterator)) {
                const char* method_name = view.api.method_get_name(method);
                if (method_name == nullptr || name != method_name) continue;
                std::string signature = describe_type(view.api, view.api.method_get_return_type(method), path) + " " + method_name + "(";
                const auto count = view.api.method_get_param_count(method);
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (index != 0) signature += ", ";
                    signature += describe_type(view.api, view.api.method_get_param(method, index), path);
                }
                signature += ')';
                INB_LOGD("method %s available: %s", path.c_str(), signature.c_str());
            }
        }
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, path,
             "no method matches the requested name and parameter count");
    }
    if constexpr (INB_LOG_ENABLED) {
        std::string key = path;
        key += '#';
        key += parameter_count.has_value() ? std::to_string(*parameter_count) : std::string("any");
        if (detail::mark_lookup_logged(context_, key)) {
            INB_LOGD("method %s -> %zu candidate(s)", path.c_str(), candidates.size());
            for (const auto& candidate : candidates) {
                std::string signature = candidate.return_type.name + " " + candidate.name + "(";
                for (std::size_t index = 0; index < candidate.parameter_types.size(); ++index) {
                    if (index != 0) signature += ", ";
                    signature += candidate.parameter_types[index].name;
                }
                signature += ')';
                INB_LOGD("method %s candidate: %s", path.c_str(), signature.c_str());
            }
        }
    }
    return Method(context_, generation_, native_, full_name_, std::string(name),
                  std::move(candidates), nullptr, image_name_, assembly_name_);
}

Method::Method(std::shared_ptr<detail::RuntimeContext> context,
               std::uint64_t generation,
               void* declaring_class,
               std::string declaring_class_name,
               std::string name,
               std::vector<MethodDescriptor> candidates,
               void* bound_instance,
               std::string image_name,
               std::string assembly_name)
    : context_(std::move(context)),
      generation_(generation),
      declaring_class_(declaring_class),
      declaring_class_name_(std::move(declaring_class_name)),
      name_(std::move(name)),
      candidates_(std::move(candidates)),
      bound_instance_(bound_instance),
      image_name_(std::move(image_name)),
      assembly_name_(std::move(assembly_name)) {}

std::string_view Method::name() const noexcept { return name_; }

std::string Method::toString() const { return declaring_class_name_ + "::" + name_; }

Method Method::overload(std::span<const std::string_view> parameter_types) const {
    const auto path = declaring_class_name_ + "::" + name_;
    (void) detail::require_scope_for(context_, generation_, path);

    std::vector<MethodDescriptor> narrowed;
    for (const auto& candidate : candidates_) {
        if (candidate.parameter_types.size() != parameter_types.size()) continue;
        bool matches = true;
        for (std::size_t index = 0; index < parameter_types.size(); ++index) {
            if (candidate.parameter_types[index].name != parameter_types[index]) {
                matches = false;
                break;
            }
        }
        if (matches) narrowed.push_back(candidate);
    }

    if (narrowed.empty()) {
        detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, path, "no overload matches the requested parameter types");
    }
    return Method(context_, generation_, declaring_class_, declaring_class_name_, name_, std::move(narrowed),
                  bound_instance_, image_name_, assembly_name_);
}

} /// namespace INB
