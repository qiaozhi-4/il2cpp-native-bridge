#include "../test_support.hpp"
#include "hooks/hook_internal.hpp"
#include "il2cpp_native_bridge/hook_slot.hpp"

#include <memory>

using namespace il2cpp_native_bridge;

namespace {

void replacement(float) {}
void trampoline(float) {}

using Slot = HookSlot<void, float>;

class StubBackend final : public HookBackend {
public:
    Result<void*> install(void*, void*) override {
        ++install_calls;
        return Result<void*>::success(reinterpret_cast<void*>(&trampoline));
    }

    Result<void> remove(void*) override {
        ++remove_calls;
        return Result<void>::success();
    }

    int install_calls{};
    int remove_calls{};
};

std::shared_ptr<detail::HookState> make_state(const std::shared_ptr<HookBackend>& backend, void* original) {
    auto state = std::make_shared<detail::HookState>();
    state->backend = backend;
    state->target = reinterpret_cast<void*>(&replacement);
    state->original.store(original, std::memory_order_release);
    return state;
}

Result<Slot::Handle> install_with(const std::shared_ptr<HookBackend>& backend, void* original) {
    return Result<Slot::Handle>::success(Slot::Handle(make_state(backend, original)));
}

} /// namespace

void run_hook_slot_tests() {
    /// 空槽位：没有原函数，移除也是成功。
    {
        Slot slot;
        test_support::check(slot.original() == nullptr, "an empty slot must not expose an original function");
        test_support::check(slot.remove().has_value(), "removing an empty slot must succeed");
        test_support::check(slot.original() == nullptr, "removing an empty slot must keep it empty");
    }

    /// 安装 → 原函数可读 → 移除 → 原函数清空 → 重复移除仍成功。
    {
        auto backend = std::make_shared<StubBackend>();
        Slot slot;
        auto installed = slot.install([&] { return install_with(backend, reinterpret_cast<void*>(&trampoline)); });
        test_support::check(installed.has_value(), "installing a valid handle must succeed");
        test_support::check(slot.original() == &trampoline, "the published trampoline must be readable without a lock");

        test_support::check(slot.remove().has_value(), "removing an installed Hook must succeed");
        test_support::check(slot.original() == nullptr, "removal must clear the published trampoline");
        test_support::check(slot.remove().has_value(), "repeated removal must succeed");
        test_support::check(backend->remove_calls == 1, "repeated removal must reach the backend exactly once");
    }

    /// 重复 install 幂等，且不会再次执行安装器。
    {
        auto backend = std::make_shared<StubBackend>();
        Slot slot;
        test_support::check(slot.install([&] { return install_with(backend, reinterpret_cast<void*>(&trampoline)); }).has_value(),
                            "the first install must succeed");

        int second_installer_calls = 0;
        auto second = slot.install([&] {
            ++second_installer_calls;
            return install_with(backend, reinterpret_cast<void*>(&trampoline));
        });
        test_support::check(second.has_value(), "a repeated install must report success");
        test_support::check(second_installer_calls == 0, "a repeated install must not run the installer again");
    }

    /// 安装器失败原样透传，槽位保持未安装。
    {
        Slot slot;
        auto failed = slot.install([] {
            return Result<Slot::Handle>::failure(make_error(ErrorCode::hook_install_failed, "backend exploded"));
        });
        test_support::check(!failed && failed.error().code == ErrorCode::hook_install_failed,
                            "installer failures must be forwarded unchanged");
        test_support::check(slot.original() == nullptr, "a failed install must not publish a trampoline");
    }

    /// 句柄未提供原函数时拒绝安装，避免替换函数拿到无法转发的空入口。
    {
        auto backend = std::make_shared<StubBackend>();
        Slot slot;
        auto rejected = slot.install([&] { return install_with(backend, nullptr); });
        test_support::check(!rejected && rejected.error().code == ErrorCode::hook_install_failed,
                            "a handle without a trampoline must be rejected");
        test_support::check(slot.original() == nullptr, "a rejected install must not publish a trampoline");
    }

    /// 析构时自动移除已安装的 Hook。
    {
        auto backend = std::make_shared<StubBackend>();
        {
            Slot slot;
            test_support::check(slot.install([&] { return install_with(backend, reinterpret_cast<void*>(&trampoline)); }).has_value(),
                                "the scoped install must succeed");
        }
        test_support::check(backend->remove_calls == 1, "the slot destructor must remove the Hook");
    }
}
