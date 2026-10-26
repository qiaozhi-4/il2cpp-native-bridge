#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

using namespace il2cpp_native_bridge;

void run_member_lookup_tests() {
    fake_il2cpp::Fixture fixture;
    auto* overloaded = fixture.add_class("Assembly-CSharp", "Probe", "Overloads");
    fixture.add_method(overloaded, "Convert", "System.Int32", {"System.Int32"}, true);
    fixture.add_method(overloaded, "Convert", "System.Single", {"System.Single"}, true);
    auto* ambiguous = fixture.add_class("Assembly-CSharp", "Probe", "Ambiguous");
    fixture.add_method(ambiguous, "Run", "System.Void", {}, true);
    fixture.add_method(ambiguous, "Run", "System.Void", {}, true);
    fixture.add_class("DuplicateA", "Probe", "Duplicate");
    fixture.add_class("DuplicateB", "Probe", "Duplicate");

    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());
    auto setter = runtime.klass("UnityEngine.Time").method<void, float>("set_timeScale");
    test_support::check(setter.found(), "managed class and method names must resolve through the fluent query");

    auto missing = runtime.klass("UnityEngine.DoesNotExist");
    test_support::check(!missing.found() && missing.error().code == ErrorCode::not_found,
                        "missing classes must return a diagnostic result");

    auto integer_overload = runtime.klass("Probe.Overloads").method<std::int32_t, std::int32_t>("Convert");
    auto float_overload = runtime.klass("Probe.Overloads").method<float, float>("Convert");
    auto wrong_overload = runtime.klass("Probe.Overloads").method<void, double>("Convert");
    test_support::check(integer_overload.found() && float_overload.found(), "exact parameter types must select overloads");
    test_support::check(!wrong_overload.found() && wrong_overload.error().code == ErrorCode::type_mismatch,
                        "an unmatched overload signature must be diagnosed");

    auto duplicate_method = runtime.klass("Probe.Ambiguous").method<void>("Run");
    test_support::check(!duplicate_method.found() && duplicate_method.error().code == ErrorCode::ambiguous_match,
                        "duplicate exact overloads must be reported as ambiguous");

    auto duplicate_class = runtime.klass("Probe.Duplicate");
    test_support::check(!duplicate_class.found() && duplicate_class.error().code == ErrorCode::ambiguous_match,
                        "a class duplicated across assemblies must be reported as ambiguous");
    auto assembly_scoped = runtime.klass("Probe.Duplicate").in_assembly("DuplicateA");
    test_support::check(assembly_scoped.found(), "assembly narrowing must resolve a duplicated class");
}
