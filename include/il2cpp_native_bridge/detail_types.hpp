#pragma once

#include "result.hpp"
#include "runtime_state.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace INB {

/**
 * C++ 类型到 IL2CPP 托管类型名的映射；无映射的类型无法参与方法/字段查询。
 *
 * 通过特化扩展新类型，特化需提供静态成员 value。
 */
template <typename T>
struct ManagedTypeName;

/// 各内建类型与托管类型名的映射特化。
template <> struct ManagedTypeName<void> { static constexpr std::string_view value = "System.Void"; };
template <> struct ManagedTypeName<bool> { static constexpr std::string_view value = "System.Boolean"; };
template <> struct ManagedTypeName<std::int8_t> { static constexpr std::string_view value = "System.SByte"; };
template <> struct ManagedTypeName<std::uint8_t> { static constexpr std::string_view value = "System.Byte"; };
template <> struct ManagedTypeName<std::int16_t> { static constexpr std::string_view value = "System.Int16"; };
template <> struct ManagedTypeName<std::uint16_t> { static constexpr std::string_view value = "System.UInt16"; };
template <> struct ManagedTypeName<std::int32_t> { static constexpr std::string_view value = "System.Int32"; };
template <> struct ManagedTypeName<std::uint32_t> { static constexpr std::string_view value = "System.UInt32"; };
template <> struct ManagedTypeName<std::int64_t> { static constexpr std::string_view value = "System.Int64"; };
template <> struct ManagedTypeName<std::uint64_t> { static constexpr std::string_view value = "System.UInt64"; };
template <> struct ManagedTypeName<float> { static constexpr std::string_view value = "System.Single"; };
template <> struct ManagedTypeName<double> { static constexpr std::string_view value = "System.Double"; };

/// 基础值类型的指针载体：用于 Hook 的 byref/ref/out 参数，value 为去掉 & 的基础名。
template <> struct ManagedTypeName<bool*> { static constexpr std::string_view value = "System.Boolean"; };
template <> struct ManagedTypeName<std::int8_t*> { static constexpr std::string_view value = "System.SByte"; };
template <> struct ManagedTypeName<std::uint8_t*> { static constexpr std::string_view value = "System.Byte"; };
template <> struct ManagedTypeName<std::int16_t*> { static constexpr std::string_view value = "System.Int16"; };
template <> struct ManagedTypeName<std::uint16_t*> { static constexpr std::string_view value = "System.UInt16"; };
template <> struct ManagedTypeName<std::int32_t*> { static constexpr std::string_view value = "System.Int32"; };
template <> struct ManagedTypeName<std::uint32_t*> { static constexpr std::string_view value = "System.UInt32"; };
template <> struct ManagedTypeName<std::int64_t*> { static constexpr std::string_view value = "System.Int64"; };
template <> struct ManagedTypeName<std::uint64_t*> { static constexpr std::string_view value = "System.UInt64"; };
template <> struct ManagedTypeName<float*> { static constexpr std::string_view value = "System.Single"; };
template <> struct ManagedTypeName<double*> { static constexpr std::string_view value = "System.Double"; };

/**
 * 免去书写 ManagedTypeName<T>::value 的便捷变量模板。
 */
template <typename T>
inline constexpr std::string_view managed_type_name = ManagedTypeName<std::remove_cvref_t<T>>::value;

} /// namespace INB
