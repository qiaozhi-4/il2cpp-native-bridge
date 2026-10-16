#include "../frida_gum_stub/fake_frida_gum_control.hpp"
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"
#include "../test_support.hpp"

using namespace il2cpp_native_bridge;

namespace {

void target_function() {}
void replacement_function() {}

} /// namespace

void run_frida_gum_backend_tests() {
    fake_frida_gum::reset();
    auto backend = make_frida_gum_hook_backend();
    test_support::check(fake_frida_gum::init_calls() == 1,
                        "Frida Gum must be initialized once before the interceptor is used");

    auto installed = backend->install(reinterpret_cast<void*>(&target_function),
                                       reinterpret_cast<void*>(&replacement_function));
    test_support::check(installed.has_value(), "Frida Gum adapter must return the original trampoline");
    test_support::check(installed.value() != nullptr, "successful replacement must expose an original trampoline");
    test_support::check(fake_frida_gum::replace_calls() == 1,
                        "Frida Gum adapter must install through Interceptor.replace");

    auto removed = backend->remove(reinterpret_cast<void*>(&target_function));
    test_support::check(removed.has_value(), "Frida Gum adapter must restore the target through Interceptor.revert");
    test_support::check(fake_frida_gum::revert_calls() == 1,
                        "Frida Gum adapter must revert exactly the requested target");
    test_support::check(fake_frida_gum::last_target() == reinterpret_cast<void*>(&target_function),
                        "Frida Gum adapter must use the same address for installation and removal");

    fake_frida_gum::reject_next_replace();
    auto rejected = backend->install(reinterpret_cast<void*>(&target_function),
                                     reinterpret_cast<void*>(&replacement_function));
    test_support::check(!rejected && rejected.error().code == ErrorCode::hook_install_failed,
                        "Gum replacement failures must be returned as a structured install error");

    fake_frida_gum::fail_flush(true);
    auto flush_failed = backend->remove(reinterpret_cast<void*>(&target_function));
    test_support::check(!flush_failed && flush_failed.error().code == ErrorCode::hook_install_failed,
                        "Gum flush failures must remain visible to the Hook owner");
}
