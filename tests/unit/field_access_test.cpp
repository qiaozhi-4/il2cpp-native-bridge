#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

using namespace il2cpp_native_bridge;

void run_field_access_tests() {
    fake_il2cpp::Fixture fixture;
    auto* klass = fixture.add_class("Assembly-CSharp", "Probe", "Fields");
    fixture.add_field(klass, "staticCount", "System.Int32", true, sizeof(std::int32_t));
    fixture.add_field(klass, "instanceCount", "System.Int32", false, sizeof(std::int32_t), 16);
    auto* object = fixture.make_object(klass);
    const auto runtime = Runtime::for_testing(fixture.api(), fixture.domain());
    const auto fields = runtime.klass("Probe.Fields");

    auto static_count = fields.field<std::int32_t>("staticCount");
    test_support::check(static_count.found(), "static field must resolve by name and type");
    test_support::check(static_count.set(37).has_value(), "static field write must succeed");
    auto static_value = static_count.get();
    test_support::check(static_value && static_value.value() == 37, "static field read must return the written value");

    auto instance_count = fields.field<std::int32_t>("instanceCount");
    test_support::check(instance_count.found(), "instance field must resolve by name and type");
    auto without_object = instance_count.get();
    test_support::check(!without_object && without_object.error().code == ErrorCode::invalid_instance,
                        "instance field access without an object must be diagnosed");
    test_support::check(instance_count.set(91, object).has_value(), "instance field write must succeed");
    auto instance_value = instance_count.get(object);
    test_support::check(instance_value && instance_value.value() == 91, "instance field read must return the written value");

    auto wrong_type = fields.field<float>("instanceCount");
    test_support::check(!wrong_type.found() && wrong_type.error().code == ErrorCode::type_mismatch,
                        "field type mismatch must be diagnosed");
    auto missing = fields.field<std::int32_t>("missing");
    test_support::check(!missing.found() && missing.error().code == ErrorCode::not_found,
                        "missing fields must be diagnosed");
}
