#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#if IL2CPP_NATIVE_BRIDGE_HAS_FRIDA_GUM
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace INB;

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

/// 从矩阵执行器注入的环境变量读取本次运行的 Unity 版本；库本身无法在运行时得知该值。
std::string expected_unity_version() {
    const char* value = std::getenv("IL2CPP_NATIVE_BRIDGE_EXPECTED_VERSION");
    return value == nullptr ? std::string{} : std::string(value);
}

std::string build_json(const std::vector<Check>& checks, std::string_view unity_version) {
    std::ostringstream output;
    /// experiment 明确标注本次是「真实运行时验收」，与原生编译产物、候选 ABI 实验区分开。
    output << "{\"schemaVersion\":1,\"experiment\":\"runtime-acceptance\",\"unityVersion\":\""
           << json_escape(unity_version) << "\",\"checks\":{";
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

/// 旧 Time smoke：单线程、旧接口，作为独立回归保留。
std::vector<Check> legacy_checks() {
    using namespace INB;
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
        return checks;
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
        return checks;
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
        return checks;
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

    return checks;
}

/// 被替换的托管方法：静态 int(int)。回调把它整体放大 40。
std::atomic<int> g_replacement_calls{0};

int replacement_overloaded(INB::ReplacementCall<int, int>& call, int value) {
    g_replacement_calls.fetch_add(1, std::memory_order_relaxed);
    return call.original(value) + 40;
}

/**
 * 新门面的验收主体：在一个 perform 里跑完整对象链。
 *
 * 失败通过返回值报告，由调用方转成 check 状态；这里不抛异常。
 */
void facade_body(std::vector<Check>& checks) {
    using namespace INB;

    /// 同名类双程序集：按 image 限定解析，两个程序集的行为必须不同。
    auto tag_a = Il2Cpp::domain().assembly("ProbeA").image()
                         .klass("Il2CppNativeBridge.Tests.SharedProbe")
                         .method("AssemblyTag", 0)
                         .invoke<Il2CppString*>();
    auto tag_b = Il2Cpp::domain().assembly("ProbeB").image()
                         .klass("Il2CppNativeBridge.Tests.SharedProbe")
                         .method("AssemblyTag", 0)
                         .invoke<Il2CppString*>();
    add(checks, "facade_dual_assembly", tag_a != nullptr && tag_b != nullptr && tag_a != tag_b ? "passed" : "failed",
        tag_a != nullptr && tag_b != nullptr ? std::string{} : "a same-named class could not be resolved per assembly");

    /// G1 参数槽：primitive 按值、引用按对象指针值、null 合法。
    auto probe = Il2Cpp::domain().assembly("Assembly-CSharp").image()
                         .klass("Il2CppNativeBridge.Tests.SourceProbe");
    const int sum = probe.method("Add", 2).invoke<int, int, int>(2, 3);
    const bool toggled = probe.method("Toggle", 1).invoke<bool, bool>(true);
    const float scaled = probe.method("Scale", 1).invoke<float, float>(1.5f);
    const double halved = probe.method("Half", 1).invoke<double, double>(4.0);
    auto* text = probe.method("Echo", 1).invoke<Il2CppString*, Il2CppString*>(tag_a);
    auto* nothing = probe.method("Identity", 1).invoke<Il2CppObject*, Il2CppObject*>(nullptr);
    const bool slots_ok = sum == 5 && !toggled && std::fabs(scaled - 3.0f) < 0.001f &&
                          std::fabs(halved - 2.0) < 0.001 && text == tag_a && nothing == nullptr;
    add(checks, "facade_invoke_slots", slots_ok ? "passed" : "failed",
        slots_ok ? std::string{} : "a primitive, reference or null slot round trip failed");

    /// 实例槽：托管实例经 obj 传入，实例方法读写自己的字段。
    auto* instance = probe.method("Create", 0).invoke<Il2CppObject*>();
    int read_back = -1;
    if (instance != nullptr) {
        probe.method("Write", 1).invoke_on<void, int>(instance, 5);
        read_back = probe.method("Read", 0).invoke_on<int>(instance);
    }
    const bool instance_ok = instance != nullptr && read_back == 5;
    add(checks, "facade_instance_slot", instance_ok ? "passed" : "failed",
        instance_ok ? std::string{}
                    : std::string("instance=") + (instance == nullptr ? "null" : "ok") +
                      " read_back=" + std::to_string(read_back));

    /// 托管替换：implementation → 回调生效 → original 转发 → revert 后回到原实现。
    const std::string_view int_parameters[] = {"System.Int32"};
    auto overloaded = Il2Cpp::domain().assembly("Assembly-CSharp").image()
                              .klass("Il2CppNativeBridge.Tests.BridgeFixture")
                              .method("Overloaded", 1)
                              .overload(int_parameters);
    const int before = overloaded.invoke<int, int>(10);
    g_replacement_calls.store(0, std::memory_order_relaxed);
    overloaded.implementation(&replacement_overloaded);
    const int replaced = overloaded.invoke<int, int>(10);
    const int calls = g_replacement_calls.load(std::memory_order_relaxed);
    overloaded.revert();
    const int after = overloaded.invoke<int, int>(10);
    const bool replace_ok = before == 11 && replaced == 51 && calls == 1 && after == 11;
    add(checks, "facade_managed_replacement", replace_ok ? "passed" : "failed",
        replace_ok ? std::string{}
                   : "before=" + std::to_string(before) + " replaced=" + std::to_string(replaced) +
                     " calls=" + std::to_string(calls) + " after=" + std::to_string(after));
}

/// 异步 perform：提交立即返回，完成在另一个线程到达。
std::vector<Check> facade_checks() {
    using namespace INB;
    std::vector<Check> checks;

    std::mutex mutex;
    std::condition_variable signal;
    bool completed = false;
    std::string failure;
    std::thread::id completion_thread;
    const std::thread::id submitter = std::this_thread::get_id();

    const auto submitted = Il2Cpp::perform(
            [&] { facade_body(checks); },
            [&](Result<void> result) {
                completion_thread = std::this_thread::get_id();
                if (!result) failure = result.error().message;
                std::lock_guard lock(mutex);
                completed = true;
                signal.notify_all();
            },
            {});
    if (!submitted) {
        add(checks, "facade_async_perform", "failed", "the perform request was rejected");
        return checks;
    }

    bool finished = false;
    {
        std::unique_lock lock(mutex);
        finished = signal.wait_for(lock, std::chrono::seconds(30), [&] { return completed; });
    }
    const bool async_ok = finished && failure.empty() && completion_thread != submitter;
    add(checks, "facade_async_perform", async_ok ? "passed" : "failed",
        async_ok ? std::string{}
                 : (!finished ? "the perform request did not complete within 30s" : failure));
    return checks;
}

std::mutex g_state_mutex;
std::vector<Check> g_checks;
std::string g_result;

} /// namespace

extern "C"
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
void BridgeAcceptanceBegin() {
    auto checks = legacy_checks();
    std::lock_guard lock(g_state_mutex);
    g_checks = std::move(checks);
}

extern "C"
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const char* BridgeAcceptanceRunFacade() {
    auto checks = facade_checks();
    std::lock_guard lock(g_state_mutex);
    g_checks.insert(g_checks.end(), checks.begin(), checks.end());
    g_result = build_json(g_checks, expected_unity_version());
    return g_result.c_str();
}

extern "C"
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const char* BridgeAcceptanceFinish() {
    std::lock_guard lock(g_state_mutex);
    g_result = build_json(g_checks, expected_unity_version());
    return g_result.c_str();
}

extern "C"
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const char* RunBridgeAcceptance() {
    BridgeAcceptanceBegin();
    (void) BridgeAcceptanceRunFacade();
    return BridgeAcceptanceFinish();
}
