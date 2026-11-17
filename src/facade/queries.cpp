#include "il2cpp_native_bridge/il2cpp.hpp"

#include "facade/facade_internal.hpp"
#include "metadata/metadata_internal.hpp"

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

/// 只规范一次 `.dll` 后缀，其余部分按原样比较。
std::string canonical_assembly_name(std::string_view name) {
    constexpr std::string_view suffix = ".dll";
    if (name.ends_with(suffix)) name.remove_suffix(suffix.size());
    return std::string(name);
}

std::string join_path(std::initializer_list<std::string_view> parts) {
    std::string path;
    for (const auto part : parts) {
        if (part.empty()) continue;
        if (!path.empty()) path += '/';
        path += part;
    }
    return path;
}

std::string describe_type(const detail::Api& api, const Il2CppType* type, std::string_view path) {
    auto name = detail::managed_name(api, type);
    if (!name) {
        detail::fail(name.error().code, ErrorStage::lookup, std::string(path), name.error().message);
    }
    return std::move(name).take_value();
}

} /// namespace

Domain Il2Cpp::domain() {
    const auto& view = detail::require_access_scope();
    if (view.domain == nullptr) {
        detail::fail(ErrorCode::internal_error, ErrorStage::scope, {}, "the active scope has no IL2CPP domain");
    }
    return Domain(view.context, view.generation, view.domain);
}

Assembly Domain::assembly(std::string_view name) const {
    const auto requested = canonical_assembly_name(name);
    const auto& view = detail::require_scope_for(context_, generation_, requested);
    if (view.api.domain_get_assemblies == nullptr || view.api.assembly_get_image == nullptr ||
        view.api.image_get_name == nullptr) {
        detail::fail(ErrorCode::missing_api, ErrorStage::lookup, requested, "assembly enumeration exports are unavailable");
    }

    std::size_t count = 0;
    const Il2CppAssembly** assemblies = view.api.domain_get_assemblies(native_, &count);
    if (assemblies == nullptr || count == 0) {
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, requested, "the IL2CPP domain has no loaded assemblies");
    }

    const Il2CppAssembly* matched = nullptr;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* image = view.api.assembly_get_image(assemblies[index]);
        const char* image_name = image == nullptr ? nullptr : view.api.image_get_name(image);
        if (image_name == nullptr) continue;
        if (canonical_assembly_name(image_name) != requested) continue;
        if (matched != nullptr) {
            detail::fail(ErrorCode::ambiguous_match, ErrorStage::lookup, requested,
                 "more than one loaded assembly matches the requested name");
        }
        matched = assemblies[index];
    }
    if (matched == nullptr) {
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, requested, "no loaded assembly matches the requested name");
    }
    return Assembly(context_, generation_, matched, requested);
}

Image Assembly::image() const {
    const auto& view = detail::require_scope_for(context_, generation_, name_);
    const auto* image = view.api.assembly_get_image(native_);
    if (image == nullptr) {
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, name_, "the assembly has no loaded image");
    }
    const char* image_name = view.api.image_get_name(image);
    return Image(context_, generation_, image, image_name == nullptr ? name_ : std::string(image_name), name_);
}

Class Image::klass(std::string_view full_name) const {
    const auto path = join_path({assembly_name_, name_, full_name});
    const auto& view = detail::require_scope_for(context_, generation_, path);
    if (view.api.class_from_name == nullptr) {
        detail::fail(ErrorCode::missing_api, ErrorStage::lookup, path, "class lookup export is unavailable");
    }

    const auto separator = full_name.rfind('.');
    const std::string name_space = separator == std::string_view::npos
            ? std::string()
            : std::string(full_name.substr(0, separator));
    const std::string short_name = separator == std::string_view::npos
            ? std::string(full_name)
            : std::string(full_name.substr(separator + 1));

    Il2CppClass* klass = view.api.class_from_name(native_, name_space.c_str(), short_name.c_str());
    if (klass == nullptr) {
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, path,
             "the selected image does not contain the requested class");
    }
    return Class(context_, generation_, klass, std::string(full_name), name_, assembly_name_);
}

Class::Class(std::shared_ptr<detail::RuntimeContext> context,
             std::uint64_t generation,
             Il2CppClass* native,
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
        detail::fail(ErrorCode::missing_api, ErrorStage::lookup, path, "method enumeration exports are unavailable");
    }

    std::vector<MethodDescriptor> candidates;
    void* iterator = nullptr;
    while (const Il2CppMethodInfo* method = view.api.class_get_methods(native_, &iterator)) {
        const char* method_name = view.api.method_get_name(method);
        if (method_name == nullptr || name != method_name) continue;

        const std::uint32_t native_parameter_count = view.api.method_get_param_count(method);
        if (parameter_count.has_value() && parameter_count.value() != native_parameter_count) continue;

        MethodDescriptor descriptor;
        descriptor.native = method;
        descriptor.declaring_class = native_;
        descriptor.declaring_class_name = full_name_;
        descriptor.name = method_name;
        descriptor.is_static = (view.api.method_get_flags(method, nullptr) & detail::kMethodAttributeStatic) != 0U;
        descriptor.return_type.native = view.api.method_get_return_type(method);
        descriptor.return_type.name = describe_type(view.api, descriptor.return_type.native, path);
        descriptor.parameter_types.reserve(native_parameter_count);
        for (std::uint32_t index = 0; index < native_parameter_count; ++index) {
            const auto* parameter_type = view.api.method_get_param(method, index);
            descriptor.parameter_types.push_back(
                    TypeDescriptor{parameter_type, describe_type(view.api, parameter_type, path)});
        }
        candidates.push_back(std::move(descriptor));
    }

    if (candidates.empty()) {
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, path,
             "no method matches the requested name and parameter count");
    }
    return Method(context_, generation_, native_, full_name_, std::string(name), std::move(candidates));
}

Method::Method(std::shared_ptr<detail::RuntimeContext> context,
               std::uint64_t generation,
               Il2CppClass* declaring_class,
               std::string declaring_class_name,
               std::string name,
               std::vector<MethodDescriptor> candidates)
    : context_(std::move(context)),
      generation_(generation),
      declaring_class_(declaring_class),
      declaring_class_name_(std::move(declaring_class_name)),
      name_(std::move(name)),
      candidates_(std::move(candidates)) {}

std::string_view Method::name() const noexcept { return name_; }

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
        detail::fail(ErrorCode::not_found, ErrorStage::lookup, path, "no overload matches the requested parameter types");
    }
    return Method(context_, generation_, declaring_class_, declaring_class_name_, name_, std::move(narrowed));
}

} /// namespace INB
