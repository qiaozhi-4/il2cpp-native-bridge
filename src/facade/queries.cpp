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

        std::string join_path(std::initializer_list<std::string_view> parts) {
            std::string path;
            for (const auto part : parts) {
                if (part.empty()) continue;
                if (!path.empty()) path += '/';
                path += part;
            }
            return path;
        }

        /// 失败定位用的完整路径；只在出错分支调用，正常路径不为它分配。
        std::string method_path(std::string_view assembly,
                                std::string_view image,
                                std::string_view klass,
                                std::string_view name) {
            return join_path({assembly, image, klass}) + "::" + std::string(name);
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
        const auto& view = detail::require_scope_for(context_, generation_, name);
        if (view.api.domain_get_assemblies == nullptr || view.api.assembly_get_image == nullptr ||
            view.api.image_get_name == nullptr) {
            detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, std::string(name),
                                     "assembly enumeration exports are unavailable");
        }

        /// 程序集枚举是 O(已加载程序集数)，且每次都要两次导出调用；按名字缓存命中即可。
        auto cache_key = detail::metadata_cache_key("assembly", generation_, nullptr);
        detail::append_metadata_cache_part(cache_key, name);
        if (auto* cached = detail::find_cached_metadata(context_, cache_key); cached != nullptr) {
            return Assembly(context_, generation_, cached, std::string(name));
        }

        std::size_t count = 0;
        const Il2CppAssembly** assemblies = view.api.domain_get_assemblies(static_cast<Il2CppDomain*>(native_), &count);
        if (assemblies == nullptr || count == 0) {
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, std::string(name),
                                     "the IL2CPP domain has no loaded assemblies");
        }

        const Il2CppAssembly* matched = nullptr;
        for (std::size_t index = 0; index < count; ++index) {
            const auto* image = view.api.assembly_get_image(assemblies[index]);
            const char* image_name = image == nullptr ? nullptr : view.api.image_get_name(image);
            if (image_name == nullptr) continue;
            if (!detail::assembly_name_matches(image_name, name)) continue;
            if (matched != nullptr) {
                detail::report_and_throw(ErrorCode::ambiguous_match, ErrorStage::lookup, std::string(name),
                     "more than one loaded assembly matches the requested name");
            }
            matched = assemblies[index];
        }
        if (matched == nullptr) {
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, std::string(name),
                                     "no loaded assembly matches the requested name");
        }
        detail::cache_metadata(context_, std::move(cache_key), const_cast<Il2CppAssembly*>(matched));
        return Assembly(context_, generation_, detail::opaque(matched), std::string(name));
    }

    Image Assembly::image() const {
        const auto& view = detail::require_scope_for(context_, generation_, name_);
        auto cache_key = detail::metadata_cache_key("image", generation_, nullptr);
        detail::append_metadata_cache_part(cache_key, name_);

        auto* image = static_cast<Il2CppImage*>(detail::find_cached_metadata(context_, cache_key));
        if (image == nullptr) {
            image = const_cast<Il2CppImage*>(view.api.assembly_get_image(static_cast<Il2CppAssembly*>(native_)));
            if (image == nullptr) {
                detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, name_, "the assembly has no loaded image");
            }
            detail::cache_metadata(context_, std::move(cache_key), image);
        }
        const char* image_name = view.api.image_get_name(image);
        return Image(context_, generation_, detail::opaque(image),
                     image_name == nullptr ? name_ : std::string(image_name), name_);
    }

    Class Image::klass(std::string_view full_name) const {
        const auto& view = detail::require_scope_for(context_, generation_, full_name);
        if (view.api.class_from_name == nullptr) {
            detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup,
                                     join_path({assembly_name_, name_, full_name}), "class lookup export is unavailable");
        }

        /// 同一镜像内按完整类名缓存；类名拆分与 class_from_name 都只在未命中时做。
        auto cache_key = detail::metadata_cache_key("image_class", generation_, nullptr);
        detail::append_metadata_cache_part(cache_key, name_);
        detail::append_metadata_cache_part(cache_key, full_name);
        if (auto* cached = detail::find_cached_metadata(context_, cache_key); cached != nullptr) {
            return Class(context_, generation_, cached, std::string(full_name), name_, assembly_name_);
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
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup,
                                     join_path({assembly_name_, name_, full_name}),
                                     "the selected image does not contain the requested class");
        }
        detail::cache_metadata(context_, std::move(cache_key), klass);
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
        /// 组装完整路径只为了出错时定位；正常路径只用方法短名，避免每次调用都分配。
        const auto& view = detail::require_scope_for(context_, generation_, name);
        if (view.api.class_get_methods == nullptr || view.api.method_get_name == nullptr ||
            view.api.method_get_param_count == nullptr || view.api.method_get_param == nullptr ||
            view.api.method_get_return_type == nullptr || view.api.method_get_flags == nullptr) {
            detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup,
                                     method_path(assembly_name_, image_name_, full_name_, name),
                                     "method enumeration exports are unavailable");
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

                MethodDescriptor descriptor = detail::descriptor_for(view, method, klass, name);
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
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup,
                                     method_path(assembly_name_, image_name_, full_name_, name),
                                     "no method matches the requested name and parameter count");
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
        (void) detail::require_scope_for(context_, generation_, name_);

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
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup,
                                     declaring_class_name_ + "::" + name_,
                                     "no overload matches the requested parameter types");
        }
        return Method(context_, generation_, declaring_class_, declaring_class_name_, name_, std::move(narrowed),
                      bound_instance_, image_name_, assembly_name_);
    }

} /// namespace INB
