// 最小消费者：只用公开门面，不手写初始化、线程附加、backend 或 HookSlot。
//
// 演示「一次 perform + 完整对象链 + 一次替换」：
//   Il2Cpp::perform → domain → assembly → image → klass → method → invoke / implementation / revert
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstdio>

namespace {

/// 替换回调：把时间缩放放大一倍后交给原实现。
/// 必须是普通函数（无捕获），这样安装时不需要额外的上下文对象。
void on_set_time_scale(il2cpp_native_bridge::ReplacementCall<void, float>& call, float value) {
    call.original(value * 2.0f);
}

} /// namespace

int main() {
    using namespace il2cpp_native_bridge;

    const auto submitted = Il2Cpp::perform(
            [] {
                auto time = Il2Cpp::domain()
                                    .assembly("UnityEngine.CoreModule")
                                    .image()
                                    .klass("UnityEngine.Time");
                auto setter = time.method("set_timeScale", 1);
                auto getter = time.method("get_timeScale", 0);

                setter.invoke<void, float>(0.5f);
                const float observed = getter.invoke<float>();
                std::printf("timeScale=%.3f\n", static_cast<double>(observed));

                /// OFF 构建没有 Gum 后端：查询与调用照常，替换按 missing_api 拒绝。
                try {
                    setter.implementation(&on_set_time_scale);
                } catch (const BridgeError& error) {
                    if (error.error().code == ErrorCode::missing_api) {
                        std::fputs("managed replacement is unavailable in this build\n", stdout);
                        return;
                    }
                    throw;
                }

                setter.invoke<void, float>(0.5f);
                setter.revert();
                setter.invoke<void, float>(observed);
            },
            [](Result<void> result) {
                if (!result) {
                    std::fprintf(stderr, "perform failed: %s\n", result.error().message.c_str());
                }
            });

    return submitted ? 0 : 1;
}
