#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "hooks/hook_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

using namespace INB;

namespace {

void replacement(float) {}

class FakeHookBackend final : public HookBackend {
public:
    Result<void*> install(void*, void*) override {
        ++install_calls;
        return Result<void*>::success(reinterpret_cast<void*>(0x2000));
    }

    Result<void> remove(void*) override {
        ++remove_calls;
        return Result<void>::success();
    }

    int install_calls{};
    int remove_calls{};
};

} /// namespace

void run_hook_lifecycle_tests() {
    fake_il2cpp::Fixture fixture;
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());
    auto setter = runtime.klass("UnityEngine.Time").method<void, float>("set_timeScale");
    auto no_backend = setter.hook({}, replacement);
    test_support::check(!no_backend && no_backend.error().code == ErrorCode::hook_backend_unavailable,
                        "Hook request without a backend must fail diagnostically");

    auto backend = std::make_shared<FakeHookBackend>();
    auto unavailable = setter.hook(backend, replacement);
    test_support::check(!unavailable && unavailable.error().code == ErrorCode::entry_point_unavailable,
                        "Hook must fail when runtime method pointer validation is unavailable");
    test_support::check(backend->install_calls == 0, "backend must not receive an unvalidated method pointer");

    auto state = std::make_shared<detail::HookState>();
    state->backend = backend;
    state->target = reinterpret_cast<void*>(0x1000);
    state->original = reinterpret_cast<void*>(0x2000);
    test_support::check(detail::remove_hook(state).has_value(), "Hook removal must succeed");
    test_support::check(detail::remove_hook(state).has_value(), "Hook removal must be idempotent");
    test_support::check(backend->remove_calls == 1, "idempotent removal must call the backend once");
    test_support::check(detail::hook_original(state.get()) == nullptr, "removed Hook must not expose a stale trampoline");
}
