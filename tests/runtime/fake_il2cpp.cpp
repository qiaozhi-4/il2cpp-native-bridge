#include "fake_il2cpp.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <set>

namespace {
Il2CppThread fake_thread;
/// 与真实 IL2CPP 一致：线程附加状态是每线程独立的。
thread_local Il2CppThread* current_thread{};
fake_il2cpp::Fixture* active_fixture{};
/// 会被多线程测试并发递增，因此用原子类型。
std::atomic<std::size_t> class_lookup_calls{};
/// lookup_symbol 需要屏蔽的符号，模拟部分导出缺失的运行时。
std::set<std::string> disabled_symbols;

Il2CppDomain* domain_get() { return active_fixture == nullptr ? nullptr : active_fixture->domain(); }
const Il2CppAssembly** domain_get_assemblies(const Il2CppDomain* domain, std::size_t* count) {
    if (domain == nullptr || count == nullptr) return nullptr;
    if (active_fixture != nullptr && !active_fixture->assemblies_available()) {
        *count = 0;
        return nullptr;
    }
    *count = domain->assemblies.size();
    return domain->assemblies.empty() ? nullptr : const_cast<const Il2CppAssembly**>(domain->assemblies.data());
}
const Il2CppImage* assembly_get_image(const Il2CppAssembly* assembly) { return assembly == nullptr ? nullptr : assembly->image; }
const char* image_get_name(const Il2CppImage* image) { return image == nullptr ? nullptr : image->name.c_str(); }
Il2CppClass* class_from_name(const Il2CppImage* image, const char* name_space, const char* name) {
    ++class_lookup_calls;
    if (image == nullptr || name_space == nullptr || name == nullptr) return nullptr;
    for (auto* klass : image->classes) {
        if (klass->name_space == name_space && klass->name == name) return klass;
    }
    return nullptr;
}
const char* class_get_name(Il2CppClass* klass) { return klass == nullptr ? nullptr : klass->name.c_str(); }
const char* class_get_namespace(Il2CppClass* klass) { return klass == nullptr ? nullptr : klass->name_space.c_str(); }
const Il2CppMethodInfo* class_get_methods(Il2CppClass* klass, void** iterator) {
    if (klass == nullptr || iterator == nullptr) return nullptr;
    auto index = reinterpret_cast<std::uintptr_t>(*iterator);
    if (index >= klass->methods.size()) return nullptr;
    *iterator = reinterpret_cast<void*>(index + 1);
    return klass->methods[index];
}
const char* method_get_name(const Il2CppMethodInfo* method) { return method == nullptr ? nullptr : method->name.c_str(); }
std::uint32_t method_get_param_count(const Il2CppMethodInfo* method) { return method == nullptr ? 0 : method->parameter_types.size(); }
const Il2CppType* method_get_param(const Il2CppMethodInfo* method, std::uint32_t index) {
    return method == nullptr || index >= method->parameter_types.size() ? nullptr : &method->parameter_types[index];
}
const Il2CppType* method_get_return_type(const Il2CppMethodInfo* method) { return method == nullptr ? nullptr : &method->return_type; }
char* type_get_name(const Il2CppType* type) {
    if (type == nullptr) return nullptr;
    auto* result = new (std::nothrow) char[type->name.size() + 1];
    if (result != nullptr) std::memcpy(result, type->name.c_str(), type->name.size() + 1);
    return result;
}
std::uint32_t method_get_flags(const Il2CppMethodInfo* method, std::uint32_t* implementation_flags) {
    if (implementation_flags != nullptr) *implementation_flags = 0;
    return method != nullptr && method->is_static ? 0x0010U : 0U;
}
const Il2CppFieldInfo* class_get_fields(Il2CppClass* klass, void** iterator) {
    if (klass == nullptr || iterator == nullptr) return nullptr;
    auto index = reinterpret_cast<std::uintptr_t>(*iterator);
    if (index >= klass->fields.size()) return nullptr;
    *iterator = reinterpret_cast<void*>(index + 1);
    return klass->fields[index];
}
const char* field_get_name(const Il2CppFieldInfo* field) { return field == nullptr ? nullptr : field->name.c_str(); }
const Il2CppType* field_get_type(const Il2CppFieldInfo* field) { return field == nullptr ? nullptr : &field->type; }
std::uint32_t field_get_flags(const Il2CppFieldInfo* field) { return field != nullptr && field->is_static ? 0x0010U : 0U; }
void field_static_get_value(const Il2CppFieldInfo* field, void* destination) {
    if (field != nullptr && destination != nullptr) std::memcpy(destination, field->static_value.data(), field->static_value.size());
}
void field_static_set_value(const Il2CppFieldInfo* field, void* source) {
    if (field != nullptr && source != nullptr) std::memcpy(const_cast<std::byte*>(field->static_value.data()), source, field->static_value.size());
}
void field_get_value(Il2CppObject* object, const Il2CppFieldInfo* field, void* destination) {
    if (object != nullptr && field != nullptr && destination != nullptr) std::memcpy(destination, object->bytes.data() + field->offset, sizeof(int));
}
void field_set_value(Il2CppObject* object, const Il2CppFieldInfo* field, void* source) {
    if (object != nullptr && field != nullptr && source != nullptr) std::memcpy(object->bytes.data() + field->offset, source, sizeof(int));
}
Il2CppObject* runtime_invoke(const Il2CppMethodInfo* method, void* instance, void** arguments, Il2CppObject** exception) {
    if (exception != nullptr) *exception = nullptr;
    if (method == nullptr || !method->invoke) return nullptr;
    return method->invoke(static_cast<Il2CppObject*>(instance), arguments, exception);
}
void* object_unbox(Il2CppObject* object) { return object == nullptr ? nullptr : object->bytes.data(); }
Il2CppClass* object_get_class(Il2CppObject* object) { return object == nullptr ? nullptr : object->klass; }
Il2CppThread* thread_current() { return current_thread; }
Il2CppThread* thread_attach(Il2CppDomain*) { current_thread = &fake_thread; return current_thread; }
void thread_detach(Il2CppThread* thread) { if (thread == current_thread) current_thread = nullptr; }
bool type_is_byref(const Il2CppType* type) { return type != nullptr && type->name.ends_with('&'); }
Il2CppClass* class_from_type(const Il2CppType* type) {
    if (type == nullptr || active_fixture == nullptr) return nullptr;
    return active_fixture->find_class(type->name);
}
bool class_is_valuetype(Il2CppClass* klass) { return klass != nullptr && klass->is_value_type; }
bool class_is_assignable_from(Il2CppClass* klass, Il2CppClass* other) {
    if (klass == nullptr || other == nullptr) return false;
    for (auto* current = other; current != nullptr; current = current->base) {
        if (current == klass) return true;
    }
    return false;
}
bool method_is_generic(const Il2CppMethodInfo* method) { return method != nullptr && method->is_generic; }
bool method_is_inflated(const Il2CppMethodInfo* method) { return method != nullptr && method->is_inflated; }
std::int32_t string_length(Il2CppString* string) {
    return string == nullptr ? 0 : static_cast<std::int32_t>(string->text.size());
}
const std::uint16_t* string_chars(Il2CppString* string) {
    if (string == nullptr || string->text.empty()) return nullptr;
    return reinterpret_cast<const std::uint16_t*>(string->text.data());
}
const Il2CppImage* get_corlib() {
    if (active_fixture == nullptr || !active_fixture->corlib_available()) return nullptr;
    return active_fixture->corlib_image();
}

} /// namespace

namespace fake_il2cpp {

Fixture::Fixture() {
    active_fixture = this;
    auto* time = add_class("UnityEngine.CoreModule", "UnityEngine", "Time");
    add_method(time, "set_timeScale", "System.Void", {"System.Single"}, true,
        [this](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
            time_scale_ = *static_cast<float*>(args[0]);
            return nullptr;
        });
    add_method(time, "get_timeScale", "System.Single", {}, true,
        [this](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* {
            auto* box = make_object(nullptr);
            std::memcpy(box->bytes.data(), &time_scale_, sizeof(time_scale_));
            return box;
        });

    /// profile bootstrap 的版本来源；默认报告 Unity 6000.3。
    application_ = add_class("UnityEngine.CoreModule", "UnityEngine", "Application");
    unity_version_method_ = add_method(application_, "get_unityVersion", "System.String", {}, true,
        [this](Il2CppObject*, void**, Il2CppObject** exception) -> Il2CppObject* {
            if (unity_version_exception_) {
                if (exception != nullptr) *exception = make_object(nullptr);
                return nullptr;
            }
            if (!unity_version_.has_value()) return nullptr;
            return make_string(*unity_version_);
        });
}

il2cpp_native_bridge::detail::Api Fixture::api() const {
    il2cpp_native_bridge::detail::Api result;
    result.domain_get = domain_get;
    result.domain_get_assemblies = domain_get_assemblies;
    result.assembly_get_image = assembly_get_image;
    result.image_get_name = image_get_name;
    result.class_from_name = class_from_name;
    result.class_get_name = class_get_name;
    result.class_get_namespace = class_get_namespace;
    result.class_get_methods = class_get_methods;
    result.method_get_name = method_get_name;
    result.method_get_param_count = method_get_param_count;
    result.method_get_param = method_get_param;
    result.method_get_return_type = method_get_return_type;
    result.type_get_name = type_get_name;
    result.method_get_flags = method_get_flags;
    result.class_get_fields = class_get_fields;
    result.field_get_name = field_get_name;
    result.field_get_type = field_get_type;
    result.field_get_flags = field_get_flags;
    result.field_static_get_value = field_static_get_value;
    result.field_static_set_value = field_static_set_value;
    result.field_get_value = field_get_value;
    result.field_set_value = field_set_value;
    result.runtime_invoke = runtime_invoke;
    result.object_unbox = object_unbox;
    result.thread_current = thread_current;
    result.thread_attach = thread_attach;
    result.thread_detach = thread_detach;
    result.get_corlib = get_corlib;
    result.type_is_byref = type_is_byref;
    result.object_get_class = object_get_class;
    result.class_from_type = class_from_type;
    result.class_is_valuetype = class_is_valuetype;
    result.class_is_assignable_from = class_is_assignable_from;
    result.method_is_generic = method_is_generic;
    result.method_is_inflated = method_is_inflated;
    result.string_length = string_length;
    result.string_chars = string_chars;
    result.free_memory = [](void* pointer) { delete[] static_cast<char*>(pointer); };
    return result;
}

Il2CppClass* Fixture::add_class(std::string assembly_name, std::string name_space, std::string name) {
    auto assembly_it = std::find_if(assemblies_.begin(), assemblies_.end(), [&](const auto& assembly) {
        return assembly.image != nullptr && assembly.image->name == assembly_name;
    });
    if (assembly_it == assemblies_.end()) {
        images_.push_back(Il2CppImage{std::move(assembly_name), {}});
        assemblies_.push_back(Il2CppAssembly{&images_.back()});
        domain_.assemblies.push_back(&assemblies_.back());
        assembly_it = assemblies_.end() - 1;
    }
    classes_.push_back(Il2CppClass{std::move(name_space), std::move(name), {}, {}});
    auto* klass = &classes_.back();
    assembly_it->image->classes.push_back(klass);
    return klass;
}

Il2CppAssembly* Fixture::add_duplicate_assembly(std::string image_name) {
    images_.push_back(Il2CppImage{std::move(image_name), {}});
    assemblies_.push_back(Il2CppAssembly{&images_.back()});
    domain_.assemblies.push_back(&assemblies_.back());
    return &assemblies_.back();
}

Il2CppMethodInfo* Fixture::add_method(Il2CppClass* klass, std::string name, std::string return_type,
                                     std::vector<std::string> parameters, bool is_static,
                                     std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke,
                                     bool is_generic) {
    std::vector<Il2CppType> types;
    for (auto& parameter : parameters) types.push_back(Il2CppType{std::move(parameter)});
    methods_.push_back(Il2CppMethodInfo{nullptr, std::move(name), Il2CppType{std::move(return_type)}, std::move(types), is_static, std::move(invoke), is_generic, false});
    auto* method = &methods_.back();
    klass->methods.push_back(method);
    return method;
}

Il2CppFieldInfo* Fixture::add_field(Il2CppClass* klass, std::string name, std::string type,
                                   bool is_static, std::size_t size, std::size_t offset) {
    fields_.push_back(Il2CppFieldInfo{std::move(name), Il2CppType{std::move(type)}, is_static, offset,
                                      std::vector<std::byte>(size)});
    auto* field = &fields_.back();
    klass->fields.push_back(field);
    return field;
}

Il2CppObject* Fixture::make_object(Il2CppClass* klass) {
    objects_.push_back(Il2CppObject{});
    objects_.back().klass = klass;
    return &objects_.back();
}

void Fixture::set_method_entry(Il2CppMethodInfo* method, void* entry) {
    if (method != nullptr) method->method_pointer = entry;
}

Il2CppObject* Fixture::make_string(std::u16string text) {
    auto* object = make_object(nullptr);
    object->text = std::move(text);
    return object;
}

void Fixture::set_unity_version(std::optional<std::u16string> version) { unity_version_ = std::move(version); }

void Fixture::set_unity_version_exception(bool raised) { unity_version_exception_ = raised; }

void Fixture::remove_unity_version_method() {
    if (application_ == nullptr || unity_version_method_ == nullptr) return;
    auto& methods = application_->methods;
    methods.erase(std::remove(methods.begin(), methods.end(), unity_version_method_), methods.end());
    unity_version_method_ = nullptr;
}

Il2CppObject* Fixture::box(const void* data, std::size_t size) {
    auto* object = make_object(nullptr);
    if (data != nullptr && size <= object->bytes.size()) std::memcpy(object->bytes.data(), data, size);
    return object;
}

void Fixture::add_corlib_types() {
    auto* object = add_class("mscorlib.dll", "System", "Object");
    auto* string_type = add_class("mscorlib.dll", "System", "String");
    string_type->base = object;
    const char* value_types[] = {"Boolean", "SByte", "Byte", "Int16", "UInt16", "Int32", "UInt32",
                                 "Int64", "UInt64", "Single", "Double", "Void"};
    for (const char* name : value_types) add_class("mscorlib.dll", "System", name)->is_value_type = true;
}

Il2CppClass* Fixture::find_class(std::string_view full_name) {
    const auto dot = full_name.rfind('.');
    const std::string_view name_space = dot == std::string_view::npos ? std::string_view{} : full_name.substr(0, dot);
    const std::string_view name = dot == std::string_view::npos ? full_name : full_name.substr(dot + 1);
    for (auto& klass : classes_) {
        if (klass.name == name && klass.name_space == name_space) return &klass;
    }
    return nullptr;
}

int Fixture::instance_count(const Il2CppObject* object, std::size_t offset) const {
    int value = 0;
    std::memcpy(&value, object->bytes.data() + offset, sizeof(value));
    return value;
}

std::size_t Fixture::class_lookup_count() const { return class_lookup_calls.load(std::memory_order_relaxed); }

void Fixture::reset_class_lookup_count() { class_lookup_calls.store(0, std::memory_order_relaxed); }

void* lookup_symbol(void*, const char* symbol_name) {
    if (active_fixture == nullptr || symbol_name == nullptr) return nullptr;
    if (disabled_symbols.contains(symbol_name)) return nullptr;
    const auto api = active_fixture->api();

#define IL2CPP_MATCH_REQUIRED(member, symbol) \
    if (std::strcmp(symbol_name, symbol) == 0) return reinterpret_cast<void*>(api.member);
#define IL2CPP_MATCH_OPTIONAL(member, symbol, group) \
    if (std::strcmp(symbol_name, symbol) == 0) return reinterpret_cast<void*>(api.member);

    IL2CPP_NATIVE_BRIDGE_EXPORT_LIST(IL2CPP_MATCH_REQUIRED, IL2CPP_MATCH_OPTIONAL)

#undef IL2CPP_MATCH_REQUIRED
#undef IL2CPP_MATCH_OPTIONAL
    return nullptr;
}

void disable_symbol(std::string name) { disabled_symbols.insert(std::move(name)); }

void clear_disabled_symbols() { disabled_symbols.clear(); }

} /// namespace fake_il2cpp
