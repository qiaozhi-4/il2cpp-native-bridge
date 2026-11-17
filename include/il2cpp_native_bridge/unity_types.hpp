#pragma once

#include "bridge.hpp"

#include <string_view>

namespace il2cpp_native_bridge {

/**
 * 与 UnityEngine.Vector2 内存布局一致的二维浮点向量。
 *
 * 用于托管方法的按值传参与返回。IL2CPP 将它作为两个连续的 float 传递，
 * 因此字段顺序、数量和大小不能调整；这里只描述内存布局，不提供 C++ 运算接口。
 */
struct UnityVector2 {
    /// 水平分量。
    float x;
    /// 垂直分量。
    float y;
};

/// 布局若被改动，替换回调与托管方法之间会按错位参数读写，且编译器不会报错。
static_assert(sizeof(UnityVector2) == 2 * sizeof(float));
static_assert(alignof(UnityVector2) == alignof(float));

/// 映射为所有 Unity 版本共有的托管类型名，避免各游戏重复定义该映射。
template <>
struct ManagedTypeName<UnityVector2> {
    static constexpr std::string_view value = "UnityEngine.Vector2";
};

} /// namespace il2cpp_native_bridge
