/// 验证静态存储期的 Hook 句柄在进程退出时析构，不会访问已被销毁的注册表。
///
/// 缺陷只在进程退出阶段暴露，因此该用例必须以独立进程运行：CTest 启动本可执行文件，
/// 主函数返回后静态析构链自然发生，崩溃会体现为非零退出码。

#include "hooks/hook_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

#include <cstdio>
#include <memory>
#include <optional>

using namespace INB;

namespace {

class StubBackend final : public HookBackend {
public:
    Result<void*> install(void*, void*) override {
        return Result<void*>::success(reinterpret_cast<void*>(0x2000));
    }

    Result<void> remove(void*) override {
        ++remove_calls;
        return Result<void>::success();
    }

    int remove_calls{};
};

/// 静态存储期句柄：进程退出时析构，此时 Hook 注册表必须仍然可用。
std::optional<HookHandle<void, float>> g_static_hook;

} /// namespace

int main() {
    auto backend = std::make_shared<StubBackend>();
    auto state = std::make_shared<detail::HookState>();
    state->backend = backend;
    state->target = reinterpret_cast<void*>(0x1000);
    state->original = reinterpret_cast<void*>(0x2000);

    g_static_hook.emplace(HookHandle<void, float>(state));
    if (g_static_hook->original() == nullptr) {
        std::fprintf(stderr, "static Hook handle did not expose a trampoline\n");
        return 1;
    }

    /// 返回后 g_static_hook 在静态析构阶段被销毁，进而调用 remove_hook。
    return 0;
}
