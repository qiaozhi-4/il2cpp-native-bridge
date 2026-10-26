#pragma once

#include "runtime/il2cpp_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <string>
#include <vector>

struct Il2CppType {
    std::string name;
};

struct Il2CppImage {
    std::string name;
    std::vector<Il2CppClass*> classes;
};

struct Il2CppAssembly {
    Il2CppImage* image{};
};

struct Il2CppDomain {
    std::vector<const Il2CppAssembly*> assemblies;
};

struct Il2CppClass {
    std::string name_space;
    std::string name;
    std::vector<Il2CppMethodInfo*> methods;
    std::vector<Il2CppFieldInfo*> fields;
};

struct Il2CppMethodInfo {
    std::string name;
    Il2CppType return_type;
    std::vector<Il2CppType> parameter_types;
    bool is_static{};
    std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke;
};

struct Il2CppFieldInfo {
    std::string name;
    Il2CppType type;
    bool is_static{};
    std::size_t offset{};
    std::vector<std::byte> static_value;
};

struct Il2CppObject {
    Il2CppClass* klass{};
    std::array<std::byte, 256> bytes{};
};

struct Il2CppThread {};

namespace fake_il2cpp {

class Fixture {
public:
    Fixture();

    il2cpp_native_bridge::detail::Api api() const;
    Il2CppDomain* domain() { return &domain_; }
    Il2CppClass* add_class(std::string assembly, std::string name_space, std::string name);
    Il2CppMethodInfo* add_method(Il2CppClass* klass, std::string name, std::string return_type,
                                 std::vector<std::string> parameters, bool is_static,
                                 std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke = {});
    Il2CppFieldInfo* add_field(Il2CppClass* klass, std::string name, std::string type,
                               bool is_static, std::size_t size, std::size_t offset = 0);
    Il2CppObject* make_object(Il2CppClass* klass);

    float time_scale() const { return time_scale_; }
    int instance_count(const Il2CppObject* object, std::size_t offset) const;

    /// 累计 class_from_name 调用次数，用于断言类解析是否命中缓存。
    std::size_t class_lookup_count() const;
    /// 清零 class_from_name 计数。
    void reset_class_lookup_count();

private:
    Il2CppDomain domain_;
    std::deque<Il2CppAssembly> assemblies_;
    std::deque<Il2CppImage> images_;
    std::deque<Il2CppClass> classes_;
    std::deque<Il2CppMethodInfo> methods_;
    std::deque<Il2CppFieldInfo> fields_;
    std::deque<Il2CppObject> objects_;
    float time_scale_{1.0f};
};

} /// namespace fake_il2cpp
