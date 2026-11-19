#include "il2cpp_native_bridge/il2cpp.hpp"

#include "facade/facade_internal.hpp"
#include "metadata/metadata_internal.hpp"
#include "runtime/il2cpp_api.hpp"

#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace INB {
    namespace detail {
        namespace {

            /// 描述符缓存的键：代际 + MethodInfo + 方法被解析时所在的类。
            using DescriptorKey = std::tuple<std::uint64_t, const void*, const void*>;

            /// 描述符缓存上限；超过后整体清空，避免长期运行积累。
            inline constexpr std::size_t kDescriptorCacheLimit = 4096;

            using DescriptorCache = LookupCache<DescriptorKey, MethodDescriptor, std::map<DescriptorKey, MethodDescriptor>>;

            DescriptorCache& descriptor_cache() {
                static DescriptorCache cache{kDescriptorCacheLimit};
                return cache;
            }

            /// 绑定缓存的键：代际 + context + MethodInfo + C++ 签名地址。
            using BindingKey = std::tuple<std::uint64_t, const void*, const void*, const void*>;

            /// 绑定缓存上限；超过后整体清空。
            inline constexpr std::size_t kBindingCacheLimit = 2048;

            using BindingCache = LookupCache<BindingKey,
                                             std::shared_ptr<const BoundSignature>,
                                             std::map<BindingKey, std::shared_ptr<const BoundSignature>>>;

            BindingCache& binding_cache() {
                static BindingCache cache{kBindingCacheLimit};
                return cache;
            }

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

        } /// namespace

        /// 方法元数据 → 描述符；成员类型名不可用时在首次发现处报错，避免参数列表被静默截短。
        MethodDescriptor descriptor_for(const ReadyView& view,
                                        const Il2CppMethodInfo* method,
                                        Il2CppClass* declaring_class,
                                        std::string_view path) {
            /// 元数据在一次 runtime 代际内不变，同一 MethodInfo 只枚举一次。
            const DescriptorKey key{view.generation, method, declaring_class};
            descriptor_cache().advance(view.generation);
            if (auto cached = descriptor_cache().find(key)) return *cached;

            MethodDescriptor descriptor;
            descriptor.native = detail::opaque(method);
            descriptor.declaring_class = declaring_class;
            const char* method_name = view.api.method_get_name(method);
            descriptor.name = method_name == nullptr ? std::string{} : std::string(method_name);

            const auto* return_type = view.api.method_get_return_type(method);
            auto return_name = managed_name(view.api, return_type);
            if (!return_name) {
                report_and_throw(return_name.error().code, return_name.error().stage, std::string(path),
                                 return_name.error().message);
            }
            descriptor.return_type.native = detail::opaque(return_type);
            descriptor.return_type.name = return_name.take_value();

            const std::uint32_t count = view.api.method_get_param_count(method);
            descriptor.parameter_types.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto* type = view.api.method_get_param(method, index);
                auto name = managed_name(view.api, type);
                if (!name) {
                    report_and_throw(name.error().code, name.error().stage, std::string(path), name.error().message);
                }
                descriptor.parameter_types.push_back(TypeDescriptor{detail::opaque(type), name.take_value()});
            }
            /// 返回值是方法标志（MethodAttributes），出参是实现标志（MethodImplAttributes）：
            /// 后者的 0x10 是 ForwardRef，不是 Static，判静态只能用返回值。
            std::uint32_t implementation_flags = 0;
            const std::uint32_t method_flags = view.api.method_get_flags(method, &implementation_flags);
            descriptor.is_static = (method_flags & kMethodAttributeStatic) != 0;
            descriptor_cache().insert(key, descriptor);
            return descriptor;
        }

        namespace {

            /// 按完整签名调用 System.Array 的成员；GetValue 与 get_Length 都有同名重载，不能按参数个数选。
            void* invoke_array_member(const std::shared_ptr<RuntimeContext>& context,
                                      Il2CppObject* array,
                                      std::string_view name,
                                      std::string_view return_type,
                                      const std::vector<std::string>& parameter_types,
                                      std::span<void*> arguments,
                                      std::string_view path) {
                auto klass = resolve_class(context, "System.Array", std::nullopt);
                if (!klass) {
                    report_and_throw(klass.error().code, klass.error().stage, std::string(path), klass.error().message);
                }
                auto method = resolve_method(klass.value(), name, return_type, parameter_types);
                if (!method) {
                    report_and_throw(method.error().code, method.error().stage, std::string(path), method.error().message);
                }
                auto invoked = invoke_raw(method.value(), array, arguments);
                if (!invoked) {
                    report_and_throw(invoked.error().code, invoked.error().stage, std::string(path), invoked.error().message);
                }
                return invoked.value().value;
            }

            /// 按类指针反查所属镜像名；仅当 Class 未携带镜像名时使用。
            std::string image_name_of_class(const ReadyView& view, Il2CppClass* native, std::string_view full_name) {
                if (view.api.domain_get_assemblies == nullptr || view.api.assembly_get_image == nullptr ||
                    view.api.image_get_name == nullptr || view.api.class_from_name == nullptr) {
                    return {};
                }
                const auto separator = full_name.rfind('.');
                const std::string name_space = separator == std::string_view::npos
                        ? std::string{}
                        : std::string(full_name.substr(0, separator));
                const std::string short_name = separator == std::string_view::npos
                        ? std::string(full_name)
                        : std::string(full_name.substr(separator + 1));

                std::size_t count = 0;
                const auto* assemblies = view.api.domain_get_assemblies(view.domain, &count);
                if (assemblies == nullptr) return {};
                for (std::size_t index = 0; index < count; ++index) {
                    const auto* image = assemblies[index] == nullptr ? nullptr : view.api.assembly_get_image(assemblies[index]);
                    if (image == nullptr) continue;
                    if (view.api.class_from_name(image, name_space.c_str(), short_name.c_str()) != native) continue;
                    const char* name = view.api.image_get_name(image);
                    return name == nullptr ? std::string{} : std::string(name);
                }
                return {};
            }

            /// 解析对象的运行期类名；对象为空或类信息不可用时在首次发现处报错。
            std::string require_object_class_name(const std::shared_ptr<RuntimeContext>& context,
                                                  std::uint64_t generation,
                                                  void* object,
                                                  std::string_view path,
                                                  std::string_view view_name) {
                (void) require_scope_for(context, generation, path);
                if (object == nullptr) {
                    report_and_throw(ErrorCode::invalid_argument, ErrorStage::lookup, std::string(path),
                                     "a null reference has no " + std::string(view_name) + " view");
                }
                auto resolved = resolve_object_class(context, static_cast<Il2CppObject*>(object));
                if (!resolved) {
                    report_and_throw(resolved.error().code, resolved.error().stage, std::string(path), resolved.error().message);
                }
                return resolved.value().full_name;
            }

        } /// namespace
    } /// namespace detail

    void* detail::unbox_data(const std::shared_ptr<RuntimeContext>& context,
                             std::uint64_t generation,
                             Il2CppObject* boxed,
                             std::string_view path) {
        const auto& view = require_scope_for(context, generation, path);
        if (view.api.object_unbox == nullptr) {
            detail::report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, std::string(path),
                                     "il2cpp_object_unbox is unavailable");
        }
        void* data = boxed == nullptr ? nullptr : view.api.object_unbox(boxed);
        if (data == nullptr) {
            detail::report_and_throw(ErrorCode::invocation_failed, ErrorStage::lookup, std::string(path),
                                     "the boxed value could not be unboxed");
        }
        return data;
    }

    std::shared_ptr<const detail::BoundSignature> detail::load_binding(const std::shared_ptr<RuntimeContext>& context,
                                                                        std::uint64_t generation,
                                                                        const void* method,
                                                                        const void* signature) {
        auto& cache = detail::binding_cache();
        cache.advance(generation);
        auto cached = cache.find(BindingKey{generation, context.get(), method, signature});
        return cached.has_value() ? *cached : nullptr;
    }

    void detail::save_binding(const std::shared_ptr<RuntimeContext>& context,
                              std::uint64_t generation,
                              const void* method,
                              const void* signature,
                              std::shared_ptr<const BoundSignature> binding) {
        auto& cache = detail::binding_cache();
        cache.advance(generation);
        cache.insert(BindingKey{generation, context.get(), method, signature}, std::move(binding));
    }

    void detail::require_field_value_type(std::string_view name, std::string_view actual, std::string_view expected) {
        /// 旧构造路径可能没有字段类型名；此时无据可依，交给运行时的字节拷贝。
        if (actual.empty() || actual == expected) return;
        detail::report_and_throw(ErrorCode::type_mismatch, ErrorStage::lookup, std::string(name),
                                 "the field type '" + std::string(actual) + "' does not match the requested C++ type '" +
                                         std::string(expected) + "'");
    }

    void detail::require_string_object(const std::shared_ptr<RuntimeContext>& context,
                                       std::uint64_t generation,
                                       void* object,
                                       std::string_view path) {
        const auto class_name = require_object_class_name(context, generation, object, path, "string");
        if (class_name != "System.String") {
            detail::report_and_throw(ErrorCode::type_mismatch, ErrorStage::lookup, std::string(path),
                                     "the object is not a managed string: " + class_name);
        }
    }

    void detail::require_array_object(const std::shared_ptr<RuntimeContext>& context,
                                      std::uint64_t generation,
                                      void* object,
                                      std::string_view path) {
        auto class_name = require_object_class_name(context, generation, object, path, "array");
        if (!class_name.ends_with("[]")) {
            detail::report_and_throw(ErrorCode::type_mismatch, ErrorStage::lookup, std::string(path),
                                     "the object is not a managed array: " + class_name);
        }
    }

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
        std::string image_name = image_name_;
        if (image_name.empty()) {
            /// 从 Il2Cpp::klass() / Object::klass() 得到的 Class 不带镜像名，按类指针反查。
            image_name = detail::image_name_of_class(view, static_cast<Il2CppClass*>(native_), full_name_);
        }
        const auto* native = detail::find_image(view, image_name);
        if (native == nullptr) {
            detail::report_and_throw(ErrorCode::not_found, ErrorStage::lookup, full_name_,
                                     "the image of this class is not loaded");
        }
        return Image(context_, generation_, detail::opaque(native), image_name, assembly_name_);
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
            auto descriptor = detail::descriptor_for(view, method, static_cast<Il2CppClass*>(native_), full_name_);
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
                auto descriptor = detail::descriptor_for(view, method, handle.value().native, class_name_);
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
        auto descriptor = detail::descriptor_for(view, resolved.value().native, resolved.value().declaring_class.native,
                                                 class_name_ + "::" + std::string(name));
        std::vector<MethodDescriptor> candidates{descriptor};
        return Method(context_, generation_, resolved.value().declaring_class.native,
                      resolved.value().declaring_class.full_name, std::string(name),
                      std::move(candidates), native_);
    }

    String Object::as_string() const {
        detail::require_string_object(context_, generation_, native_,
                                      class_name_.empty() ? std::string_view{"object::as_string"}
                                                          : std::string_view{class_name_});
        return String(context_, generation_, reinterpret_cast<Il2CppString*>(native_));
    }

    GcHandle Object::ref(bool pin) const {
        return GcHandle(context_, generation_, detail::gchandle_new(context_, generation_, native_, pin, false));
    }

    GcHandle Object::weak_ref(bool pin) const {
        return GcHandle(context_, generation_, detail::gchandle_new(context_, generation_, native_, pin, true));
    }

    std::string Object::toString() const {
        if (native_ == nullptr) return {};
        auto resolved = detail::resolve_object_class(context_, static_cast<Il2CppObject*>(native_));
        return resolved ? resolved.value().full_name : class_name_;
    }

    String::String(std::shared_ptr<detail::RuntimeContext> context, std::uint64_t generation, void* native)
        : context_(std::move(context)), generation_(generation), native_(native) {}

    std::string String::content() const { return detail::string_content(context_, generation_, native_); }

    Object String::object() const {
        return Object(context_, generation_, reinterpret_cast<Il2CppObject*>(native_), nullptr, "System.String");
    }

    Object GcHandle::target() const {
        auto* raw = detail::gchandle_target(context_, generation_, handle_);
        return Object(context_, generation_, raw, nullptr, {});
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
            /// 释放不校验代际：旧代际的句柄同样必须归还，否则每次 runtime 重建都漏一批。
            if (current_access_scope() == nullptr) return;
            const auto& view = require_access_scope();
            if (view.context != context) return;
            if (view.api.gchandle_free != nullptr) view.api.gchandle_free(handle);
        }

        void* gchandle_target(const std::shared_ptr<RuntimeContext>& context, std::uint64_t generation, std::uint32_t handle) {
            const auto& view = require_scope_for(context, generation, "gchandle::target");
            if (view.api.gchandle_get_target == nullptr) {
                report_and_throw(ErrorCode::missing_api, ErrorStage::invoke, "gchandle::target",
                                 "il2cpp_gchandle_get_target is unavailable");
            }
            return view.api.gchandle_get_target(handle);
        }

        std::string string_content(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   void* value) {
            const auto& view = require_scope_for(context, generation, "string::content");
            if (value == nullptr) {
                report_and_throw(ErrorCode::invalid_argument, ErrorStage::lookup, "string::content",
                                 "a null reference has no string content");
            }
            if (view.api.string_length == nullptr || view.api.string_chars == nullptr) {
                report_and_throw(ErrorCode::missing_api, ErrorStage::lookup, "string::content",
                                 "il2cpp string reading exports are unavailable");
            }
            const auto length = view.api.string_length(static_cast<Il2CppString*>(value));
            if (length < 0) {
                report_and_throw(ErrorCode::invocation_failed, ErrorStage::lookup, "string::content",
                                 "the managed string returned a negative length");
            }
            if (length == 0) return {};
            const auto* chars = view.api.string_chars(static_cast<Il2CppString*>(value));
            if (chars == nullptr) {
                report_and_throw(ErrorCode::invocation_failed, ErrorStage::lookup, "string::content",
                                 "the managed string returned no character data");
            }
            return utf16_to_utf8(
                    std::u16string_view(reinterpret_cast<const char16_t*>(chars), static_cast<std::size_t>(length)));
        }

        std::size_t array_length(const std::shared_ptr<RuntimeContext>& context,
                                 std::uint64_t generation,
                                 void* array) {
            const auto& view = require_scope_for(context, generation, "array::length");
            if (array == nullptr) {
                report_and_throw(ErrorCode::invalid_argument, ErrorStage::lookup, "array::length",
                                 "a null reference has no array length");
            }
            if (view.api.array_length != nullptr) {
                return view.api.array_length(static_cast<Il2CppArray*>(array));
            }

            /// 导出缺失时回退到 System.Array.get_Length；走内部解析以复用元数据缓存。
            const auto length = invoke_array_member(context, static_cast<Il2CppObject*>(array), "get_Length",
                                                    "System.Int32", {}, {}, "array::length");
            void* value = unbox_data(context, generation, static_cast<Il2CppObject*>(length), "array::length");
            std::int32_t count = 0;
            std::memcpy(&count, value, sizeof(count));
            if (count < 0) {
                report_and_throw(ErrorCode::invocation_failed, ErrorStage::lookup, "array::length",
                                 "the managed array returned a negative length");
            }
            return static_cast<std::size_t>(count);
        }

        void* array_element(const std::shared_ptr<RuntimeContext>& context,
                            std::uint64_t generation,
                            void* array,
                            std::size_t index) {
            const auto& view = require_scope_for(context, generation, "array::at");
            if (array == nullptr) {
                report_and_throw(ErrorCode::invalid_argument, ErrorStage::lookup, "array::at",
                                 "a null reference has no elements");
            }
            /// 有长度导出时先拦越界，避免把越界变成托管异常再回传。
            if (view.api.array_length != nullptr &&
                index >= view.api.array_length(static_cast<Il2CppArray*>(array))) {
                report_and_throw(ErrorCode::invalid_argument, ErrorStage::lookup, "array::at",
                                 "the element index is out of range");
            }
            std::int32_t managed_index = static_cast<std::int32_t>(index);
            void* arguments[] = {&managed_index};
            return invoke_array_member(context, static_cast<Il2CppObject*>(array), "GetValue",
                                       "System.Object", {"System.Int32"}, arguments, "array::at");
        }

        void* unbox_value(const std::shared_ptr<RuntimeContext>& context,
                          std::uint64_t generation,
                          void* boxed) {
            if (boxed == nullptr) return nullptr;
            return unbox_data(context, generation, static_cast<Il2CppObject*>(boxed), "value::unbox");
        }

    } /// namespace detail
} /// namespace INB
