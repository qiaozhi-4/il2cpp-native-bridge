#pragma once

#include "il2cpp_native_bridge/result.hpp"

#include <cstdint>
#include <string>

namespace il2cpp_native_bridge::detail {

struct ReadyView;

/// profile 的证据等级。
enum class ProfileSupport {
    /// 没有任何证据：拒绝安装替换。
    rejected,
    /// 只有源码证据（T031），缺运行时证据（T044）：可内部安装，不声称支持。
    candidate,
    /// 源码与运行时证据齐备：可安装并声称支持。
    approved,
};

/**
 * 一个 Unity 版本 family 的原生入口形态及其证据等级。
 *
 * 形态字段只记录已有证据的结论；证据不足的 family 不填形态，直接拒绝。
 */
struct AbiProfile {
    /// bootstrap 读到的原始版本串；未 bootstrap 时为空。
    std::string unity_version;
    /// "modern-static" / "legacy-static-slot"；空表示没有 family 覆盖该版本。
    std::string family;
    /// MethodInfo 内实现入口指针的字节偏移。
    std::uint32_t entry_offset{};
    /// 静态方法是否隐含首个实例槽（legacy family 的特征）。
    bool dummy_instance_static{};
    /// native 末位是否带隐藏 MethodInfo。
    bool hidden_method_info{};
    /// 证据等级；只有 candidate 及以上允许安装替换。
    ProfileSupport support{ProfileSupport::rejected};
};

/**
 * 按需 bootstrap 当前运行时的 profile。
 *
 * 只用已校验的 runtime_invoke 调用 `UnityEngine.Application.get_unityVersion`，
 * 不依赖 Gum，也不参与普通查询与 invoke。同一 context 不缓存结果：安装替换是低频操作，
 * 缓存反而引入 generation 失效与锁序问题。
 */
Result<AbiProfile> resolve_abi_profile(const ReadyView& view);

} /// namespace il2cpp_native_bridge::detail
