#pragma once

#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/runtime_internal.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace INB::detail {

    /// IL2CPP 方法标志位中的 static 位。
    /// 只对 il2cpp_method_get_flags 的返回值有效：它的出参是 MethodImplAttributes，0x10 在那里表示 ForwardRef。
    inline constexpr std::uint32_t kMethodAttributeStatic = 0x0010;
    /// IL2CPP 字段标志位中的 static 位。
    inline constexpr std::uint32_t kFieldAttributeStatic = 0x0010;

    /// 一次托管调用返回的原始对象指针：值类型为装箱结果，void 方法与 null 返回值都为空。
    struct InvokeOutcome {
        Il2CppObject* value{};
    };

    /// 校验类句柄非空、context 有效且代际匹配。
    Result<void> validate_class_handle(const ClassHandle& klass);
    /// 校验方法句柄及其声明类。
    Result<void> validate_method_handle(const MethodHandle& method);
    /// 校验字段句柄及其声明类。
    Result<void> validate_field_handle(const FieldHandle& field);
    /// 读取托管类型名，剥离 "class "/"valuetype " 前缀并规范化 void。
    Result<std::string> managed_name(const Api& api, const Il2CppType* type);

    /// 托管字符串的 UTF-16 内容转 UTF-8；非法代理对替换为 U+FFFD。
    std::string utf16_to_utf8(std::u16string_view value);

    /// 判断镜像名与请求的程序集名是否指同一程序集，容忍 ".dll" 后缀差异。
    bool assembly_name_matches(std::string_view image_name, std::string_view requested_name);

    /// 按完整名解析唯一类；可选程序集名用于消除同名歧义。
    Result<ClassHandle> resolve_class(const std::shared_ptr<RuntimeContext>& context,
                                      std::string_view full_name,
                                      const std::optional<std::string>& assembly_name);
    /// 枚举所有类，按名称子串过滤并按名称与程序集排序。
    Result<std::vector<ClassHandle>> resolve_classes(const std::shared_ptr<RuntimeContext>& context,
                                                     std::string_view name_filter);
    /// 按泛型定义与类型实参反射出封闭泛型类。
    Result<ClassHandle> resolve_generic_class(const std::shared_ptr<RuntimeContext>& context,
                                              std::string_view generic_type_name,
                                              std::span<const std::string_view> type_arguments,
                                              const std::optional<std::string>& assembly_name);
    /// 读取托管对象实际所属的类。
    Result<ClassHandle> resolve_object_class(const std::shared_ptr<RuntimeContext>& context, Il2CppObject* instance);
    /// 枚举类的全部方法元数据。
    Result<std::vector<MethodMetadata>> resolve_methods(const ClassHandle& klass);
    /// 枚举类的全部字段句柄。
    Result<std::vector<FieldHandle>> resolve_fields(const ClassHandle& klass);
    /// 分配该类的一个新托管实例。
    Result<Il2CppObject*> allocate_object(const ClassHandle& klass);

    /// 按名字、返回类型与参数类型解析唯一方法。
    Result<MethodHandle> resolve_method(const ClassHandle& klass,
                                        std::string_view name,
                                        std::string_view return_type,
                                        const std::vector<std::string>& parameter_types);
    /// 按名字与参数个数解析唯一方法，不核对类型名。
    Result<MethodHandle> resolve_method_by_arity(const ClassHandle& klass,
                                                 std::string_view name,
                                                 std::size_t parameter_count);
    /// 按名字与可选类型解析唯一字段；类型名为空时不核对类型。
    Result<FieldHandle> resolve_field(const ClassHandle& klass,
                                      std::string_view name,
                                      std::string_view value_type);

    /// 按字节读取字段到 destination。
    Result<void> read_field_raw(const FieldHandle& field, Il2CppObject* instance, void* destination, std::size_t size);
    /// 按字节从 source 写入字段。
    Result<void> write_field_raw(const FieldHandle& field, Il2CppObject* instance, void* source, std::size_t size);
    /// 将装箱值拆箱后写入字段。
    Result<void> write_boxed_field(const FieldHandle& field, Il2CppObject* boxed_value, Il2CppObject* instance);

    /// 调用已解析方法，返回原始托管结果。
    Result<InvokeOutcome> invoke_raw(const MethodHandle& method, Il2CppObject* instance, std::span<void*> arguments);

    /// 缓存键的前缀：类别 + 代际 + 拥有者地址，后续用 append_metadata_cache_part 追加名字。
    inline std::string metadata_cache_key(std::string_view kind,
                                          std::uint64_t generation,
                                          const void* owner) {
        return std::string(kind) + ':' + std::to_string(generation) + ':' +
               std::to_string(reinterpret_cast<std::uintptr_t>(owner));
    }

    /// 追加带长度前缀的一段；名字里含分隔符时不会和别的键拼成同一个键。
    inline void append_metadata_cache_part(std::string& key, std::string_view value) {
        key.push_back('|');
        key += std::to_string(value.size());
        key.push_back(':');
        key.append(value);
    }

} /// namespace INB::detail
