#pragma once

#include "runtime/il2cpp_api.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
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
    bool is_value_type{};
    Il2CppClass* base{};
};

struct Il2CppMethodInfo {
    /// 必须位于最前：入口解析按 MethodInfo 首字段读取实现入口。
    void* method_pointer{};
    std::string name;
    Il2CppType return_type;
    std::vector<Il2CppType> parameter_types;
    bool is_static{};
    std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke;
    bool is_generic{};
    bool is_inflated{};
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
    /// 托管字符串内容；只有字符串对象使用。
    std::u16string text;
};

/// 托管字符串就是带 text 的对象；测试里用 reinterpret_cast 在两者间转换。
struct Il2CppString : Il2CppObject {};

struct Il2CppThread {};

namespace fake_il2cpp {

class Fixture {
public:
    Fixture();

    il2cpp_native_bridge::detail::Api api() const;
    Il2CppDomain* domain() { return &domain_; }
    Il2CppClass* add_class(std::string assembly, std::string name_space, std::string name);
    /// 追加一个与已有 image 同名的新程序集，用于构造「同名程序集歧义」。
    Il2CppAssembly* add_duplicate_assembly(std::string image_name);
    Il2CppMethodInfo* add_method(Il2CppClass* klass, std::string name, std::string return_type,
                                 std::vector<std::string> parameters, bool is_static,
                                 std::function<Il2CppObject*(Il2CppObject*, void**, Il2CppObject**)> invoke = {},
                                 bool is_generic = false);
    Il2CppFieldInfo* add_field(Il2CppClass* klass, std::string name, std::string type,
                               bool is_static, std::size_t size, std::size_t offset = 0);
    /// 设置方法的实现入口；不设置时入口解析会因不可执行而失败。
    void set_method_entry(Il2CppMethodInfo* method, void* entry);
    Il2CppObject* make_object(Il2CppClass* klass);
    /// 造一个装箱对象；用于让 fake 的返回值符合「值类型装箱」的真实约定。
    Il2CppObject* box(const void* data, std::size_t size);
    /// 造一个托管字符串对象，供 string_chars/string_length 读取。
    Il2CppObject* make_string(std::u16string text);
    /// 设置 `Application.get_unityVersion` 的返回值；空值表示返回 null（不抛异常）。
    void set_unity_version(std::optional<std::u16string> version);
    /// 让 `get_unityVersion` 抛托管异常。
    void set_unity_version_exception(bool raised);
    /// 移除 `get_unityVersion`，用于构造「运行时没有该 getter」。
    void remove_unity_version_method();
    /// 注册 mscorlib 的常用类型，供类型识别与赋值兼容用例使用。
    void add_corlib_types();
    Il2CppClass* find_class(std::string_view full_name);

    float time_scale() const { return time_scale_; }
    int instance_count(const Il2CppObject* object, std::size_t offset) const;

    /// 累计 class_from_name 调用次数，用于断言类解析是否命中缓存。
    std::size_t class_lookup_count() const;
    /// 清零 class_from_name 计数。
    void reset_class_lookup_count();

    /// 控制 il2cpp_get_corlib 是否返回有效镜像，用于构造「运行时尚未初始化」。
    void set_corlib_available(bool available) { corlib_available_.store(available); }
    [[nodiscard]] bool corlib_available() const noexcept { return corlib_available_.load(); }
    [[nodiscard]] Il2CppImage* corlib_image() noexcept { return &corlib_; }
    /// 控制 domain_get_assemblies 是否已能看到程序集，用于构造「已加载但未就绪」。
    void set_assemblies_available(bool available) { assemblies_available_.store(available); }
    [[nodiscard]] bool assemblies_available() const noexcept { return assemblies_available_.load(); }

private:
    Il2CppDomain domain_;
    std::deque<Il2CppAssembly> assemblies_;
    std::deque<Il2CppImage> images_;
    std::deque<Il2CppClass> classes_;
    std::deque<Il2CppMethodInfo> methods_;
    std::deque<Il2CppFieldInfo> fields_;
    std::deque<Il2CppObject> objects_;
    Il2CppImage corlib_{"mscorlib.dll", {}};
    Il2CppClass* application_{};
    Il2CppMethodInfo* unity_version_method_{};
    std::optional<std::u16string> unity_version_{u"6000.3.10f1"};
    bool unity_version_exception_{};
    /// 会被协调线程并发读取，因此用原子类型。
    std::atomic<bool> corlib_available_{true};
    std::atomic<bool> assemblies_available_{true};
    float time_scale_{1.0f};
};

/// 按符号名解析当前 Fixture 的导出表，供非注入式 runtime 使用。
void* lookup_symbol(void* module, const char* symbol_name);
/// 让 lookup_symbol 对指定符号返回空指针，用于构造能力缺失。
void disable_symbol(std::string name);
/// 清空 disable_symbol 累积的屏蔽列表。
void clear_disabled_symbols();

} /// namespace fake_il2cpp
