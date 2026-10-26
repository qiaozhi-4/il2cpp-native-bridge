#include "../test_support.hpp"
#include "../runtime/fake_il2cpp.hpp"
#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/il2cpp_api.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

Il2CppDomain* fake_domain_get() { return nullptr; }
const Il2CppAssembly** fake_domain_get_assemblies(const Il2CppDomain*, std::size_t*) { return nullptr; }

void* lookup_partial(void*, const char* name) {
    if (std::strcmp(name, "il2cpp_domain_get") == 0) {
        return reinterpret_cast<void*>(&fake_domain_get);
    }
    if (std::strcmp(name, "il2cpp_domain_get_assemblies") == 0) {
        return reinterpret_cast<void*>(&fake_domain_get_assemblies);
    }
    return nullptr;
}

void* lookup_none(void*, const char*) { return nullptr; }

} /// namespace

void run_runtime_lifecycle_tests() {
    const auto missing = il2cpp_native_bridge::detail::resolve_exports(nullptr, lookup_none);
    test_support::check(!missing.has_required_exports(), "an empty runtime must report missing required exports");
    test_support::check(!missing.missing_required.empty(), "missing export diagnostics must not be empty");

    const auto partial = il2cpp_native_bridge::detail::resolve_exports(nullptr, lookup_partial);
    test_support::check(partial.api.domain_get != nullptr, "resolver must bind il2cpp_domain_get");
    test_support::check(partial.api.domain_get_assemblies != nullptr, "resolver must bind domain assembly enumeration");
    test_support::check(!partial.has_required_exports(), "partial exports must not mark the runtime ready");
    test_support::check(std::find(partial.missing_required.begin(), partial.missing_required.end(), "il2cpp_runtime_invoke") != partial.missing_required.end(),
                        "missing runtime invocation must be named in diagnostics");

    const auto host_runtime = il2cpp_native_bridge::Runtime::current();
    test_support::check(host_runtime.snapshot().state == il2cpp_native_bridge::RuntimeState::waiting_for_module,
                        "host runtime without libil2cpp must remain in the waiting state");

    /// 相同参数重复配置幂等；任一参数不同则走原有路径（ready 之前允许覆盖）。
    void* module = reinterpret_cast<void*>(0x10);
    test_support::check(il2cpp_native_bridge::Runtime::set_symbol_lookup(module, lookup_none, nullptr).has_value(),
                        "the first symbol lookup configuration must succeed");
    test_support::check(il2cpp_native_bridge::Runtime::set_symbol_lookup(module, lookup_none, nullptr).has_value(),
                        "repeating an identical symbol lookup configuration must be idempotent");
    test_support::check(il2cpp_native_bridge::Runtime::set_symbol_lookup(module, lookup_partial, nullptr).has_value(),
                        "changing the resolver before the runtime is ready must still succeed");
}
