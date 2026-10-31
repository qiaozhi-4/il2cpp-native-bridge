#pragma once

#include <type_traits>
#include <utility>

namespace fake_frida_gum {

/// 清空所有状态与统计。
void reset();
/// 下一次 replace 返回失败。
void reject_next_replace();
/// flush 是否失败（模拟屏障未确认）。
void fail_flush(bool fail);
/// 模拟「仍有线程停留在 trampoline 内」：为 true 时 flush 返回 false。
void set_in_flight(bool in_flight);
/// 安装钩子：在 replace 登记目标之后、返回之前被调用，用于模拟「安装返回前已有回调」。
using InstallHook = void (*)(void* target, void* replacement, void* replacement_data);
void set_install_hook(InstallHook hook);

int init_calls();
int replace_calls();
int revert_calls();
int unref_calls();
void* last_target();
void* last_replacement();
void* last_replacement_data();

/// 记录一次「目标被调用」的作用域：作用域内 get_current_invocation 返回该目标的上下文。
class ScopedInvocation {
public:
    explicit ScopedInvocation(void* target);
    ~ScopedInvocation();
    ScopedInvocation(const ScopedInvocation&) = delete;
    ScopedInvocation& operator=(const ScopedInvocation&) = delete;
};

/// 取目标当前已安装的替换入口；未安装时为空。
void* replacement_for(void* target);

/// 设置该目标被替换后返回的「原函数跳板」；测试必须提供与真实 ABI 同形的函数。
void set_original(void* target, void* original);

/// 按真实 ABI 模拟一次调用：把参数原样传给已安装的替换入口。
template <typename Return, typename... Args>
Return invoke(void* target, Args... args) {
    ScopedInvocation scope(target);
    auto* replacement = replacement_for(target);
    if (replacement == nullptr) {
        if constexpr (!std::is_void_v<Return>) {
            return Return{};
        } else {
            return;
        }
    }
    auto callable = reinterpret_cast<Return (*)(Args...)>(replacement);
    if constexpr (std::is_void_v<Return>) {
        callable(args...);
        return;
    } else {
        return callable(args...);
    }
}

} /// namespace fake_frida_gum
