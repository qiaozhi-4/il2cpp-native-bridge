#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

#include <cstring>

using namespace il2cpp_native_bridge;

void run_invocation_tests() {
    fake_il2cpp::Fixture fixture;
    auto* instance_class = fixture.add_class("Assembly-CSharp", "Probe", "InstanceMethods");
    auto* object = fixture.make_object(instance_class);
    fixture.add_method(instance_class, "read", "System.Int32", {}, false,
        [object](Il2CppObject* instance, void**, Il2CppObject**) -> Il2CppObject* {
            auto* result = new Il2CppObject{};
            const std::int32_t value = instance == object ? 12 : -1;
            std::memcpy(result->bytes.data(), &value, sizeof(value));
            return result;
        });
    auto* failing_class = fixture.add_class("Assembly-CSharp", "Probe", "Failure");
    fixture.add_method(failing_class, "throwManaged", "System.Void", {}, true,
        [&fixture](Il2CppObject*, void**, Il2CppObject** exception) -> Il2CppObject* {
            *exception = fixture.make_object(nullptr);
            return nullptr;
        });

    const auto runtime = Runtime::for_testing(fixture.api(), fixture.domain());
    auto set_time = runtime.klass("UnityEngine.Time").method<void, float>("set_timeScale");
    auto set_result = set_time.invoke(0.625f);
    test_support::check(set_result && fixture.time_scale() == 0.625f,
                        "typed invocation must pass primitive arguments to the runtime");
    auto get_time = runtime.klass("UnityEngine.Time").method<float>("get_timeScale");
    auto get_result = get_time.invoke();
    test_support::check(get_result && get_result.value() == 0.625f,
                        "value-type results must be unboxed from runtime invocation");

    auto method = runtime.klass("Probe.InstanceMethods").method<std::int32_t>("read");
    auto static_call = method.invoke();
    test_support::check(!static_call && static_call.error().code == ErrorCode::invalid_instance,
                        "instance methods cannot be invoked without an object");
    auto instance_call = method.invoke_on(object);
    test_support::check(instance_call && instance_call.value() == 12,
                        "instance method invocation must pass the managed receiver");

    auto failure = runtime.klass("Probe.Failure").method<void>("throwManaged").invoke();
    test_support::check(!failure && failure.error().code == ErrorCode::managed_exception,
                        "managed exceptions must be returned as structured failures");
}
