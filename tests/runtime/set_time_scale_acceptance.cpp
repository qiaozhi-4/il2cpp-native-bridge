#include "il2cpp_native_bridge/bridge.hpp"

#if IL2CPP_NATIVE_BRIDGE_HAS_FRIDA_GUM
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"
#endif

#include <atomic>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace il2cpp_native_bridge;

struct Check {
    std::string name;
    std::string status;
    std::string details;
};

std::atomic<float> hook_argument{0.0f};
std::atomic<void*> original_setter{nullptr};

void replacement_set_time_scale(float value) {
    hook_argument.store(value, std::memory_order_relaxed);
    auto original = reinterpret_cast<void (*)(float)>(original_setter.load(std::memory_order_relaxed));
    if (original != nullptr) original(value);
}

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char character : input) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += character; break;
        }
    }
    return output;
}

void add(std::vector<Check>& checks, std::string name, std::string status, std::string details = {}) {
    checks.push_back(Check{std::move(name), std::move(status), std::move(details)});
}

template <typename T>
std::string describe(const Result<T>& result) {
    return result ? std::string{} : result.error().message;
}

std::string build_json(const std::vector<Check>& checks, std::string_view unity_version) {
    std::ostringstream output;
    output << "{\"schemaVersion\":1,\"unityVersion\":\"" << json_escape(unity_version)
           << "\",\"checks\":{";
    for (std::size_t index = 0; index < checks.size(); ++index) {
        if (index != 0) output << ',';
        const auto& check = checks[index];
        output << '"' << json_escape(check.name) << "\":{\"status\":\""
               << json_escape(check.status) << "\",\"details\":\""
               << json_escape(check.details) << "\"}";
    }
    output << "}}";
    return output.str();
}

std::string execute_acceptance() {
    using namespace il2cpp_native_bridge;
    std::vector<Check> checks;
    auto runtime = Runtime::current();
    auto refreshed = runtime.refresh();
    const auto snapshot = runtime.snapshot();
    if (!refreshed || snapshot.state != RuntimeState::ready) {
        const std::string reason = !refreshed ? refreshed.error().message : snapshot.last_error;
        add(checks, "runtime_discovery", "failed", reason.empty() ? "IL2CPP runtime is not ready" : reason);
        for (const char* name : {"class_lookup", "method_lookup", "overload_selection", "field_get_set", "method_invoke"}) {
            add(checks, name, "blocked", "IL2CPP runtime discovery did not complete");
        }
        add(checks, "set_timeScale_find_call_hook_remove_restore", "blocked", "IL2CPP runtime discovery did not complete");
        return build_json(checks, snapshot.unity_version);
    }
    add(checks, "runtime_discovery", "passed");

    auto time = runtime.klass("UnityEngine.Time");
    auto fixture = runtime.klass("Il2CppNativeBridge.Tests.BridgeFixture");
    if (!time.found() || !fixture.found()) {
        const std::string reason = !time.found() ? time.error().message : fixture.error().message;
        add(checks, "class_lookup", "failed", reason);
        for (const char* name : {"method_lookup", "overload_selection", "field_get_set", "method_invoke"}) {
            add(checks, name, "blocked", "required managed class was not found");
        }
        add(checks, "set_timeScale_find_call_hook_remove_restore", "blocked", "UnityEngine.Time was not found");
        return build_json(checks, snapshot.unity_version);
    }
    add(checks, "class_lookup", "passed");

    auto set_time_scale = time.method<void, float>("set_timeScale");
    auto get_time_scale = time.method<float>("get_timeScale");
    auto integer_overload = fixture.method<int, int>("Overloaded");
    auto float_overload = fixture.method<int, float>("Overloaded");
    if (!set_time_scale.found() || !get_time_scale.found()
        || !integer_overload.found() || !float_overload.found()) {
        std::string reason;
        if (!set_time_scale.found()) reason = set_time_scale.error().message;
        else if (!get_time_scale.found()) reason = get_time_scale.error().message;
        else if (!integer_overload.found()) reason = integer_overload.error().message;
        else reason = float_overload.error().message;
        add(checks, "method_lookup", "failed", reason);
        add(checks, "overload_selection", "blocked", "required methods were not found");
        add(checks, "field_get_set", "blocked", "required managed methods were not found");
        add(checks, "method_invoke", "blocked", "required managed methods were not found");
        add(checks, "set_timeScale_find_call_hook_remove_restore", "blocked", reason);
        return build_json(checks, snapshot.unity_version);
    }
    add(checks, "method_lookup", "passed");

    auto int_result = integer_overload.invoke(10);
    auto float_result = float_overload.invoke(10.0f);
    if (int_result && float_result && int_result.value() == 11 && float_result.value() == 12) {
        add(checks, "overload_selection", "passed");
    } else {
        add(checks, "overload_selection", "failed", "an exact overload returned an unexpected value");
    }

    auto field = fixture.field<int>("retainedStaticField");
    auto field_before = field.get();
    auto field_write = field.set(97);
    auto field_after = field.get();
    const bool field_ok = field_before && field_write && field_after && field_after.value() == 97;
    if (field_ok) (void)field.set(field_before.value());
    add(checks, "field_get_set", field_ok ? "passed" : "failed",
        field_ok ? std::string{} : (!field.found() ? field.error().message : "static fixture field get/set failed"));

    auto initial_time_scale = get_time_scale.invoke();
    auto set_time_scale_result = set_time_scale.invoke(0.75f);
    auto observed_time_scale = get_time_scale.invoke();
    const bool invoke_ok = initial_time_scale && set_time_scale_result && observed_time_scale
        && std::fabs(observed_time_scale.value() - 0.75f) < 0.001f;
    add(checks, "method_invoke", invoke_ok ? "passed" : "failed",
        invoke_ok ? std::string{} : "set_timeScale invocation did not update timeScale");

#if IL2CPP_NATIVE_BRIDGE_HAS_FRIDA_GUM
    bool hook_ok = false;
    std::string hook_error;
    if (initial_time_scale && invoke_ok) {
        auto backend = make_frida_gum_hook_backend();
        auto hook_result = set_time_scale.hook(std::move(backend), &replacement_set_time_scale);
        if (hook_result) {
            auto hook = hook_result.take_value();
            original_setter.store(reinterpret_cast<void*>(hook.original()), std::memory_order_relaxed);
            hook_argument.store(0.0f, std::memory_order_relaxed);
            auto hooked_call = set_time_scale.invoke(0.375f);
            auto hooked_value = get_time_scale.invoke();
            const bool callback_ok = std::fabs(hook_argument.load(std::memory_order_relaxed) - 0.375f) < 0.001f;
            const bool original_ok = hooked_value && std::fabs(hooked_value.value() - 0.375f) < 0.001f;
            auto removed = hook.remove();
            original_setter.store(nullptr, std::memory_order_relaxed);
            auto restored = set_time_scale.invoke(initial_time_scale.value());
            auto final_value = get_time_scale.invoke();
            const bool restored_ok = restored && final_value
                && std::fabs(final_value.value() - initial_time_scale.value()) < 0.001f;
            hook_ok = hooked_call && callback_ok && original_ok && removed && restored_ok;
            if (!hook_ok) hook_error = "hook callback, original trampoline, removal, or value restoration failed";
        } else {
            hook_error = hook_result.error().message;
            (void)set_time_scale.invoke(initial_time_scale.value());
        }
    } else {
        hook_error = "timeScale invocation failed before the Hook check";
    }
    add(checks, "set_timeScale_find_call_hook_remove_restore", hook_ok ? "passed" : "failed", hook_error);
#else
    (void)set_time_scale.invoke(initial_time_scale ? initial_time_scale.value() : 1.0f);
    add(checks, "set_timeScale_find_call_hook_remove_restore", "not_applicable",
        "host fixtures validate lookup and invocation; Frida Gum Hook acceptance runs on Android ARM64");
#endif

    return build_json(checks, snapshot.unity_version);
}

} /// namespace

extern "C"
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const char* RunBridgeAcceptance() {
    static thread_local std::string result;
    result = execute_acceptance();
    return result.c_str();
}
