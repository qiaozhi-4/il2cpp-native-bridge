#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

using namespace INB;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

/// 固定的探针运行时：corlib 类型 + 一个 Probe.Counter 类。
struct InvokeFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};
    Il2CppClass* string_class{};
    Il2CppClass* object_class{};
    Il2CppObject* instance{};

    InvokeFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        string_class = fixture.find_class("System.String");
        object_class = fixture.find_class("System.Object");
        instance = fixture.make_object(counter);
    }
};

/// 在作用域内解析 Probe.Counter 的指定方法并执行回调。
template <typename Callback>
Outcome run_method(const char* name, std::size_t parameter_count, Callback&& callback) {
    return run_in_scope([&](Outcome& outcome) {
        auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                              .method(name, parameter_count);
        callback(method, outcome);
    });
}

} /// namespace

void run_facade_invoke_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 静态基础类型：参数按值地址打包，返回值从装箱对象复制。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Add", "System.Int32", {"System.Int32", "System.Int32"}, true,
                           [&fixture](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               int sum = *static_cast<int*>(args[0]) + *static_cast<int*>(args[1]);
                               return fixture.box(&sum, sizeof(sum));
                           });
        fixture.add_method(context.counter, "Toggle", "System.Boolean", {"System.Boolean"}, true,
                           [&fixture](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               bool value = !*static_cast<bool*>(args[0]);
                               return fixture.box(&value, sizeof(value));
                           });
        fixture.add_method(context.counter, "Scale", "System.Single", {"System.Single"}, true,
                           [&fixture](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               float value = *static_cast<float*>(args[0]) * 2.0f;
                               return fixture.box(&value, sizeof(value));
                           });
        fixture.add_method(context.counter, "Half", "System.Double", {"System.Double"}, true,
                           [&fixture](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               double value = *static_cast<double*>(args[0]) * 0.5;
                               return fixture.box(&value, sizeof(value));
                           });
        test_support::FacadeHarness harness(fixture.api());

        const auto sum = run_method("Add", 2, [](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke<std::int32_t, std::int32_t, std::int32_t>(2, 3);
        });
        test_support::check(sum.ok, "a static primitive method must be callable");
        test_support::check(sum.int_value == 5, "the primitive arguments must be passed by value address");

        const auto toggled = run_method("Toggle", 1, [](const Method& method, Outcome& outcome) {
            outcome.bool_value = method.invoke<bool, bool>(true);
        });
        test_support::check(toggled.ok && !toggled.bool_value, "bool arguments must round-trip");

        const auto scaled = run_method("Scale", 1, [](const Method& method, Outcome& outcome) {
            outcome.float_value = method.invoke<float, float>(1.5f);
        });
        test_support::check(scaled.ok && scaled.float_value == 3.0f, "float arguments must round-trip");

        const auto halved = run_method("Half", 1, [](const Method& method, Outcome& outcome) {
            outcome.double_value = method.invoke<double, double>(3.0);
        });
        test_support::check(halved.ok && halved.double_value == 1.5, "double arguments must round-trip");
    }

    /// 引用参数传对象指针值；null 合法；引用返回直接是对象指针。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Echo", "System.String", {"System.String"}, true,
                           [](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               return static_cast<Il2CppObject*>(args[0]);
                           });
        fixture.add_method(context.counter, "Identity", "System.Object", {"System.Object"}, true,
                           [](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               return static_cast<Il2CppObject*>(args[0]);
                           });
        test_support::FacadeHarness harness(fixture.api());

        auto* text = fixture.make_object(context.string_class);
        const auto echoed = run_method("Echo", 1, [text](const Method& method, Outcome& outcome) {
            outcome.object = reinterpret_cast<Il2CppObject*>(
                    method.invoke<Il2CppString*, Il2CppString*>(reinterpret_cast<Il2CppString*>(text)));
        });
        test_support::check(echoed.ok, "a static method with a string parameter must be callable");
        test_support::check(echoed.object == text,
                            "a reference argument must be passed as the object pointer, not its address");

        const auto null_echo = run_method("Echo", 1, [](const Method& method, Outcome& outcome) {
            outcome.object = reinterpret_cast<Il2CppObject*>(
                    method.invoke<Il2CppString*, Il2CppString*>(nullptr));
        });
        test_support::check(null_echo.ok && null_echo.object == nullptr, "a null reference argument must be allowed");

        auto* payload = fixture.make_object(context.object_class);
        const auto identity = run_method("Identity", 1, [payload](const Method& method, Outcome& outcome) {
            outcome.object = method.invoke<Il2CppObject*, Il2CppObject*>(payload);
        });
        test_support::check(identity.ok && identity.object == payload, "object references must round-trip");
    }

    /// 实例方法：静态形式拒绝，实例形式按 declaring class 校验。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Read", "System.Int32", {}, false,
                           [&fixture](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* {
                               int value = 42;
                               return fixture.box(&value, sizeof(value));
                           });
        test_support::FacadeHarness harness(fixture.api());

        const auto wrong_form = run_method("Read", 0, [](const Method& method, Outcome&) {
            (void) method.invoke<std::int32_t>();
        });
        test_support::check(!wrong_form.ok && wrong_form.code == ErrorCode::invalid_instance,
                            "an instance method must not be callable through the static form");
        test_support::check(wrong_form.stage == ErrorStage::invoke,
                            "an instance mismatch must be attributed to the invoke stage");

        const auto read = run_method("Read", 0, [&context](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke_on<std::int32_t>(context.instance);
        });
        test_support::check(read.ok && read.int_value == 42, "an instance method must run with a valid object");

        auto* foreign = fixture.make_object(context.string_class);
        const auto foreign_call = run_method("Read", 0, [foreign](const Method& method, Outcome&) {
            (void) method.invoke_on<std::int32_t>(foreign);
        });
        test_support::check(!foreign_call.ok && foreign_call.code == ErrorCode::invalid_instance,
                            "an object of another class must be rejected");

        /// 子类实例可以调用基类方法（assignability 沿基类链判断）。
        auto* derived = fixture.add_class("AssemblyA", "Probe", "DerivedCounter");
        derived->base = context.counter;
        auto* derived_object = fixture.make_object(derived);
        const auto inherited = run_method("Read", 0, [derived_object](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke_on<std::int32_t>(derived_object);
        });
        test_support::check(inherited.ok && inherited.int_value == 42,
                            "a subclass instance must be accepted for a base-class method");
    }

    /// 静态方法不接受实例；返回值缺失与托管异常都要明确失败。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Ping", "System.Void", {}, true,
                           [](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* { return nullptr; });
        fixture.add_method(context.counter, "NoValue", "System.Int32", {}, true,
                           [](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* { return nullptr; });
        fixture.add_method(context.counter, "Throws", "System.Void", {}, true,
                           [&fixture](Il2CppObject*, void**, Il2CppObject** exception) -> Il2CppObject* {
                               *exception = fixture.make_object(nullptr);
                               return nullptr;
                           });
        test_support::FacadeHarness harness(fixture.api());

        const auto static_instance = run_method("Ping", 0, [&context](const Method& method, Outcome&) {
            method.invoke_on<void>(context.instance);
        });
        test_support::check(!static_instance.ok && static_instance.code == ErrorCode::invalid_instance,
                            "a static method must reject an explicit instance");

        const auto missing = run_method("NoValue", 0, [](const Method& method, Outcome&) {
            (void) method.invoke<std::int32_t>();
        });
        test_support::check(!missing.ok && missing.code == ErrorCode::invocation_failed,
                            "a missing value result must be reported as invocation_failed");

        const auto thrown = run_method("Throws", 0, [](const Method& method, Outcome&) {
            method.invoke<void>();
        });
        test_support::check(!thrown.ok && thrown.code == ErrorCode::managed_exception,
                            "a non-empty exception output must map to managed_exception");
    }

    /// 绑定期拒绝：参数个数、返回类型、byref、泛型与值类型参数。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Add", "System.Int32", {"System.Int32", "System.Int32"}, true);
        fixture.add_method(context.counter, "TakeInt", "System.Void", {"System.Int32"}, true);
        fixture.add_method(context.counter, "ByRef", "System.Void", {"System.Int32&"}, true);
        fixture.add_method(context.counter, "Generic", "System.Void", {}, true, {}, true);
        test_support::FacadeHarness harness(fixture.api());

        const auto arity = run_method("Add", 2, [](const Method& method, Outcome&) {
            (void) method.typed<std::int32_t, std::int32_t>();
        });
        test_support::check(!arity.ok && arity.code == ErrorCode::invalid_argument,
                            "a mismatched argument count must be rejected at bind time");

        const auto result = run_method("Add", 2, [](const Method& method, Outcome&) {
            (void) method.typed<float, std::int32_t, std::int32_t>();
        });
        test_support::check(!result.ok && result.code == ErrorCode::type_mismatch,
                            "a mismatched result type must be rejected");

        const auto byref = run_method("ByRef", 1, [](const Method& method, Outcome&) {
            (void) method.typed<void, std::int32_t>();
        });
        test_support::check(!byref.ok && byref.code == ErrorCode::unsupported_runtime,
                            "byref parameters must be rejected");

        const auto generic = run_method("Generic", 0, [](const Method& method, Outcome&) {
            (void) method.typed<void>();
        });
        test_support::check(!generic.ok && generic.code == ErrorCode::unsupported_runtime,
                            "generic methods must be rejected");

        const auto value_as_object = run_method("TakeInt", 1, [](const Method& method, Outcome&) {
            (void) method.typed<void, Il2CppObject*>();
        });
        test_support::check(!value_as_object.ok && value_as_object.code == ErrorCode::type_mismatch,
                            "a value-type parameter must not be bound to a reference carrier");
    }

    /// 缺 unbox 能力时明确失败，而不是返回未初始化值。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Read", "System.Int32", {}, true,
                           [](Il2CppObject*, void**, Il2CppObject**) -> Il2CppObject* { return nullptr; });
        auto api = fixture.api();
        api.object_unbox = nullptr;
        test_support::FacadeHarness harness(api);

        const auto outcome = run_method("Read", 0, [](const Method& method, Outcome&) {
            (void) method.invoke<std::int32_t>();
        });
        test_support::check(!outcome.ok && outcome.code == ErrorCode::missing_api,
                            "a missing unbox export must be reported as missing_api");
    }

    /// BoundMethod 可跨 perform 保存并复用。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        fixture.add_method(context.counter, "Add", "System.Int32", {"System.Int32", "System.Int32"}, true,
                           [&fixture](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               int sum = *static_cast<int*>(args[0]) + *static_cast<int*>(args[1]);
                               return fixture.box(&sum, sizeof(sum));
                           });
        test_support::FacadeHarness harness(fixture.api());

        std::optional<BoundMethod<std::int32_t, std::int32_t, std::int32_t>> saved;
        const auto bound = run_method("Add", 2, [&saved](const Method& method, Outcome&) {
            saved = method.typed<std::int32_t, std::int32_t, std::int32_t>();
        });
        test_support::check(bound.ok && saved.has_value(), "binding must produce a reusable object");

        const auto reused = run_in_scope([&saved](Outcome& outcome) {
            outcome.int_value = saved->invoke(4, 5);
        });
        test_support::check(reused.ok && reused.int_value == 9,
                            "a saved bound method must be callable in a later scope");
    }

    /// byref 放行契约：replacement 路径用指针载体 + 基础名匹配时接受，invoke 路径仍拒绝。
    {
        InvokeFixture context;
        auto& fixture = context.fixture;
        auto* method_info = fixture.add_method(context.counter, "MoveByRoute", "System.Void",
                                               {"System.Boolean&"}, true);
        test_support::FacadeHarness harness(fixture.api());

        MethodDescriptor descriptor;
        descriptor.native = method_info;
        descriptor.declaring_class = context.counter;
        descriptor.declaring_class_name = "Probe.Counter";
        descriptor.name = "MoveByRoute";
        descriptor.return_type = TypeDescriptor{&method_info->return_type, method_info->return_type.name};
        for (auto& parameter : method_info->parameter_types) {
            descriptor.parameter_types.push_back(TypeDescriptor{&parameter, parameter.name});
        }
        descriptor.is_static = true;

        const std::array<detail::ArgumentSpec, 1> pointer_carrier{
                detail::ArgumentSpec{true, false, "System.Boolean"}};
        const std::array<detail::ArgumentSpec, 1> value_carrier{
                detail::ArgumentSpec{false, false, "System.Boolean"}};
        const auto context_ptr = harness.context();

        const auto allowed = run_in_scope([&](Outcome& outcome) {
            const auto generation = detail::require_access_scope().generation;
            auto binding = detail::bind_method(context_ptr, generation, {descriptor},
                                               "Probe.Counter::MoveByRoute", false, false,
                                               "System.Void", pointer_carrier, true);
            (void) binding;
        });
        test_support::check(allowed.ok,
                            "a matching pointer byref carrier must bind when allow_byref is set");

        const auto denied_invoke = run_in_scope([&](Outcome& outcome) {
            const auto generation = detail::require_access_scope().generation;
            auto binding = detail::bind_method(context_ptr, generation, {descriptor},
                                               "Probe.Counter::MoveByRoute", false, false,
                                               "System.Void", value_carrier, false);
            (void) binding;
        });
        test_support::check(!denied_invoke.ok && denied_invoke.code == ErrorCode::unsupported_runtime,
                            "byref must stay rejected on the invoke path");

        const auto denied_mismatch = run_in_scope([&](Outcome& outcome) {
            const auto generation = detail::require_access_scope().generation;
            auto binding = detail::bind_method(context_ptr, generation, {descriptor},
                                               "Probe.Counter::MoveByRoute", false, false,
                                               "System.Void", value_carrier, true);
            (void) binding;
        });
        test_support::check(!denied_mismatch.ok && denied_mismatch.code == ErrorCode::unsupported_runtime,
                            "a non-pointer carrier must not bind a byref parameter");
    }
}
