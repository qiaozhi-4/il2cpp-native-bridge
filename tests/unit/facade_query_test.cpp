#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace il2cpp_native_bridge;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

} /// namespace

void run_facade_query_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 同名类分属两个程序集时只使用所选 image，不回退全域查找。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter_a = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter_a, "Read", "System.Int32", {}, true);
        auto* counter_b = fixture.add_class("AssemblyB", "Probe", "Counter");
        fixture.add_method(counter_b, "Write", "System.Void", {}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto found = run_in_scope([](Outcome& outcome) {
            auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Read", 0);
            outcome.name = std::string(method.name());
        });
        test_support::check(found.ok, "the selected image must resolve its own class");
        test_support::check(found.name == "Read", "the resolved method must be the one declared in that image");

        const auto leaked = run_in_scope([](Outcome&) {
            (void) Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Write", 0);
        });
        test_support::check(!leaked.ok && leaked.code == ErrorCode::not_found,
                            "a method that only exists in the other image must not be found");
        test_support::check(leaked.stage == ErrorStage::lookup, "a failed lookup must be attributed to the lookup stage");
    }

    /// 程序集名只规范一次 .dll 后缀，两种写法指向同一程序集。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Read", "System.Int32", {}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto bare = run_in_scope([](Outcome& outcome) {
            outcome.name = std::string(Il2Cpp::domain().assembly("AssemblyA").name());
        });
        test_support::check(bare.ok && bare.name == "AssemblyA", "the assembly name must be usable without a suffix");

        const auto suffixed = run_in_scope([](Outcome& outcome) {
            outcome.name = std::string(Il2Cpp::domain().assembly("AssemblyA.dll").name());
        });
        test_support::check(suffixed.ok && suffixed.name == "AssemblyA",
                            "a single .dll suffix must be normalized to the same assembly");
    }

    /// 未知程序集与同名程序集分别报 not_found / ambiguous_match，并保留首个失败路径。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Read", "System.Int32", {}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto missing = run_in_scope([](Outcome&) { (void) Il2Cpp::domain().assembly("Missing"); });
        test_support::check(!missing.ok && missing.code == ErrorCode::not_found, "an unknown assembly must report not_found");
        test_support::check(missing.path == "Missing", "the failure path must name the requested assembly");

        fixture.add_duplicate_assembly("AssemblyA");
        const auto ambiguous = run_in_scope([](Outcome&) { (void) Il2Cpp::domain().assembly("AssemblyA"); });
        test_support::check(!ambiguous.ok && ambiguous.code == ErrorCode::ambiguous_match,
                            "two loaded assemblies with the same name must not be resolved arbitrarily");
    }

    /// 方法筛选：0 个候选立即失败，参数个数按托管参数计。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Read", "System.Int32", {}, true);
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Int32"}, true);
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Single"}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto by_count = run_in_scope([](Outcome& outcome) {
            outcome.name = std::string(Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                               .method("Read", 0).name());
        });
        test_support::check(by_count.ok && by_count.name == "Read", "a zero-parameter method must match by count");

        const auto wrong_count = run_in_scope([](Outcome&) {
            (void) Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Read", 1);
        });
        test_support::check(!wrong_count.ok && wrong_count.code == ErrorCode::not_found,
                            "a mismatched parameter count must not fall back to another overload");

        const auto unknown = run_in_scope([](Outcome&) {
            (void) Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Nope");
        });
        test_support::check(!unknown.ok && unknown.code == ErrorCode::not_found,
                            "an unknown method name must fail immediately");
    }

    /// 同名同数量的候选可继续按参数类型缩小；没有匹配时报 not_found。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Int32"}, true);
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Single"}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto single = run_in_scope([](Outcome& outcome) {
            const std::array<std::string_view, 1> types{"System.Single"};
            outcome.name = std::string(Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                               .method("Overloaded", 1).overload(types).name());
        });
        test_support::check(single.ok && single.name == "Overloaded", "overload must narrow candidates by parameter types");

        const auto no_match = run_in_scope([](Outcome&) {
            const std::array<std::string_view, 1> types{"System.String"};
            (void) Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                    .method("Overloaded", 1).overload(types);
        });
        test_support::check(!no_match.ok && no_match.code == ErrorCode::not_found,
                            "an unmatched overload must fail instead of picking a candidate");
    }

    /// 作用域之外不能访问运行时；已保存的 Method 可以跨 perform 复用，名称读取不需要作用域。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Int32"}, true);
        fixture.add_method(counter, "Overloaded", "System.Void", {"System.Single"}, true);
        test_support::FacadeHarness harness(fixture.api());

        bool rejected = false;
        try {
            (void) Il2Cpp::domain();
        } catch (const BridgeError& error) {
            rejected = error.error().code == ErrorCode::scope_violation && error.error().stage == ErrorStage::scope;
        }
        test_support::check(rejected, "entering the chain outside a perform scope must be rejected");

        std::optional<Method> saved;
        const auto first = run_in_scope([&](Outcome& outcome) {
            saved = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Overloaded", 1);
            outcome.name = std::string(saved->name());
        });
        test_support::check(first.ok, "the first scope must resolve and save the method");
        test_support::check(saved.has_value(), "the method must be copyable out of the scope");

        const auto second = run_in_scope([&](Outcome& outcome) {
            const std::array<std::string_view, 1> types{"System.Int32"};
            outcome.name = std::string(saved->overload(types).name());
        });
        test_support::check(second.ok, "a saved method must be reusable in a later scope of the same context");

        test_support::check(saved->name() == "Overloaded", "reading the resolved name must not require a scope");
    }

    /// 来自其他 runtime context 的句柄必须被拒绝，不能跨 context 使用。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        auto* counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Read", "System.Int32", {}, true);

        std::optional<Method> saved;
        {
            test_support::FacadeHarness harness(fixture.api());
            const auto first = run_in_scope([&](Outcome&) {
                saved = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter").method("Read", 0);
            });
            test_support::check(first.ok, "the owning context must resolve the method");
        }

        test_support::FacadeHarness other(fixture.api());
        const auto reused = run_in_scope([&](Outcome&) {
            const std::array<std::string_view, 0> types{};
            (void) saved->overload(types);
        });
        test_support::check(!reused.ok && reused.code == ErrorCode::scope_violation,
                            "a handle from another runtime context must be rejected");
    }
}
