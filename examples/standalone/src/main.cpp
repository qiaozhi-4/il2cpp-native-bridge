#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"

#include <utility>

namespace {

using TimeScaleFunction = void (*)(float);
TimeScaleFunction original_time_scale{};

void on_set_time_scale(float value) {
    if (original_time_scale != nullptr) original_time_scale(value);
}

} /// namespace

int main() {
    using namespace il2cpp_native_bridge;

    auto runtime = Runtime::current();
    auto attachment = ThreadAttachment::attach(runtime);
    if (!attachment) return 1;

    auto time = runtime.klass("UnityEngine.Time");
    auto setter = time.method<void, float>("set_timeScale");
    auto getter = time.method<float>("get_timeScale");
    if (!setter.found() || !getter.found()) return 2;

    auto initial_scale = getter.invoke();
    if (!initial_scale) return 3;

    auto backend = make_frida_gum_hook_backend();
    auto hook_result = setter.hook(std::move(backend), on_set_time_scale);
    if (!hook_result) return 4;
    auto hook = std::move(hook_result.value());
    original_time_scale = hook.original();
    if (original_time_scale == nullptr) return 5;

    auto changed = setter.invoke(0.5f);
    auto observed = getter.invoke();
    if (!changed || !observed || observed.value() != 0.5f) return 6;

    auto removed = hook.remove();
    original_time_scale = nullptr;
    if (!removed) return 7;

    auto restored = setter.invoke(initial_scale.value());
    if (!restored) return 8;
    return 0;
}
