#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstdint>
#include <string>

using namespace il2cpp_native_bridge;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

struct CapabilityFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};
    Il2CppObject* instance{};

    CapabilityFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        instance = fixture.make_object(counter);
        fixture.add_method(counter, "Echo", "System.String", {"System.String"}, true,
                           [](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               return static_cast<Il2CppObject*>(args[0]);
                           });
        fixture.add_method(counter, "Count", "System.Int32", {}, true,
                           [this](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* {
                               const int value = 7;
                               return fixture.box(&value, sizeof(value));
                           });
        fixture.add_method(counter, "Take", "System.Int32", {"System.Int32"}, true,
                           [this](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               return fixture.box(args[0], sizeof(int));
                           });
    }
};

std::int32_t on_take(ReplacementCall<std::int32_t, std::int32_t>&, std::int32_t value) { return value; }

template <typename Callback>
Outcome run_method(const char* name, std::size_t parameter_count, Callback&& callback) {
    return run_in_scope([&](Outcome& outcome) {
        auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                              .method(name, parameter_count);
        callback(method, outcome);
    });
}

/// snapshot 与 core 分属两套就绪约定，读取时同样要持锁。
RuntimeState legacy_state(const std::shared_ptr<detail::RuntimeContext>& context) {
    std::lock_guard lock(context->mutex);
    return context->snapshot.state;
}

} /// namespace

void run_facade_capability_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 新 core 就绪不等于旧 legacy 就绪：门面可用，旧 Runtime 状态不变。
    {
        CapabilityFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        test_support::check(legacy_state(harness.context()) != RuntimeState::ready,
                            "the legacy snapshot must not be marked ready by the facade path");

        const auto echoed = run_method("Echo", 1, [](const Method& method, Outcome& outcome) {
            outcome.object = reinterpret_cast<Il2CppObject*>(
                    method.invoke<Il2CppString*, Il2CppString*>(nullptr));
        });
        test_support::check(echoed.ok, "a query and invoke must work without legacy readiness");
        test_support::check(test_support::core_state(harness.context()) == RuntimeState::ready,
                            "the facade must publish core readiness");
        test_support::check(legacy_state(harness.context()) != RuntimeState::ready,
                            "the legacy snapshot must stay untouched");
    }

    /// 缺 object_unbox 只阻断值返回，不阻断引用返回。
    {
        CapabilityFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        harness.context()->api.object_unbox = nullptr;

        const auto counted = run_method("Count", 0, [](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke<std::int32_t>();
        });
        test_support::check(!counted.ok && counted.code == ErrorCode::missing_api,
                            "a value result must fail with missing_api when unbox is unavailable");

        const auto echoed = run_method("Echo", 1, [](const Method& method, Outcome& outcome) {
            outcome.object = reinterpret_cast<Il2CppObject*>(
                    method.invoke<Il2CppString*, Il2CppString*>(nullptr));
        });
        test_support::check(echoed.ok, "a reference result must not depend on unbox");
    }

    /// 缺 class_from_type 只阻断对象载体校验，不阻断基础类型参数。
    {
        CapabilityFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        harness.context()->api.class_from_type = nullptr;

        const auto taken = run_method("Take", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke<std::int32_t, std::int32_t>(5);
        });
        test_support::check(taken.ok && taken.int_value == 5,
                            "a primitive parameter must not depend on class_from_type");
    }

    /// 缺 string_chars 只影响 profile bootstrap，不影响 invoke。
    {
        CapabilityFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        harness.context()->api.string_chars = nullptr;

        const auto taken = run_method("Take", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke<std::int32_t, std::int32_t>(11);
        });
        test_support::check(taken.ok && taken.int_value == 11,
                            "invoke must not depend on string access exports");
    }

    /// 缺 string_chars 时安装替换必须按 missing_api 拒绝，而不是猜版本。
    {
        CapabilityFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        harness.context()->api.string_chars = nullptr;

        const auto installed = run_method("Take", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_take);
        });
        test_support::check(!installed.ok && installed.code == ErrorCode::missing_api,
                            "profile bootstrap must report missing_api when string access is gone");
        test_support::check(installed.stage == ErrorStage::replacement,
                            "the profile failure must be reported at the replacement stage");
    }
}
