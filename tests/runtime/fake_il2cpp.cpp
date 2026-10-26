#include "fake_il2cpp.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace {
Il2CppThread fake_thread;
Il2CppThread* current_thread{};
fake_il2cpp::Fixture* active_fixture{};
std::size_t class_lookup_calls{};

Il2CppDomain* domain_get() { return active_fixture == nullptr ? nullptr : active_fixture->domain(); }
const Il2CppAssembly** domain_get_assemblies(const Il2CppDomain* domain, std::size_t* count) {
    if (domain == nullptr || count == nullptr) return nullptr;
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
Il2CppThread* thread_current() { return current_thread; }
Il2CppThread* thread_attach(Il2CppDomain*) { current_thread = &fake_thread; return current_thread; }
void thread_detach(Il2CppThread* thread) { if (thread == current_thread) current_thread = nullptr; }

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

Il2CppMethodInfo* Fixture::add_method(Il2CppClass* klass, std::string name, std::string return_type,
                                     std::vector<std::string> parameters, bool is_static,
                                     std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke) {
    std::vector<Il2CppType> types;
    for (auto& parameter : parameters) types.push_back(Il2CppType{std::move(parameter)});
    methods_.push_back(Il2CppMethodInfo{std::move(name), Il2CppType{std::move(return_type)}, std::move(types), is_static, std::move(invoke)});
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

int Fixture::instance_count(const Il2CppObject* object, std::size_t offset) const {
    int value = 0;
    std::memcpy(&value, object->bytes.data() + offset, sizeof(value));
    return value;
}

std::size_t Fixture::class_lookup_count() const { return class_lookup_calls; }

void Fixture::reset_class_lookup_count() { class_lookup_calls = 0; }

} /// namespace fake_il2cpp
