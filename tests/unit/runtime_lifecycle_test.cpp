#include "../test_support.hpp"
#include "../runtime/fake_il2cpp.hpp"
#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/il2cpp_api.hpp"
#include "runtime/runtime_internal.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
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

/// 构造一个走真实解析路径（非注入 API）的 context，用于观察 core/legacy 就绪差异。
std::shared_ptr<INB::detail::RuntimeContext> make_resolved_context() {
    auto context = INB::detail::make_testing_context({}, nullptr);
    context->module = reinterpret_cast<void*>(0x1);
    context->symbol_lookup = &fake_il2cpp::lookup_symbol;
    return context;
}

bool group_missing(const INB::detail::RuntimeContext& context, const std::string& group) {
    return context.missing_by_group.find(group) != context.missing_by_group.end();
}

} /// namespace

void run_runtime_lifecycle_tests() {
    const auto missing = INB::detail::resolve_exports(nullptr, lookup_none);
    test_support::check(!missing.has_required_exports(), "an empty runtime must report missing required exports");
    test_support::check(!missing.missing_required.empty(), "missing export diagnostics must not be empty");

    const auto partial = INB::detail::resolve_exports(nullptr, lookup_partial);
    test_support::check(partial.api.domain_get != nullptr, "resolver must bind il2cpp_domain_get");
    test_support::check(partial.api.domain_get_assemblies != nullptr, "resolver must bind domain assembly enumeration");
    test_support::check(!partial.has_required_exports(), "partial exports must not mark the runtime ready");
    test_support::check(std::find(partial.missing_required.begin(), partial.missing_required.end(), "il2cpp_runtime_invoke") != partial.missing_required.end(),
                        "missing runtime invocation must be named in diagnostics");

    const auto host_runtime = INB::Runtime::current();
    test_support::check(host_runtime.snapshot().state == INB::RuntimeState::waiting_for_module,
                        "host runtime without libil2cpp must remain in the waiting state");

    /// 相同参数重复配置幂等；任一参数不同则走原有路径（ready 之前允许覆盖）。
    void* module = reinterpret_cast<void*>(0x10);
    test_support::check(INB::Runtime::set_symbol_lookup(module, lookup_none, nullptr).has_value(),
                        "the first symbol lookup configuration must succeed");
    test_support::check(INB::Runtime::set_symbol_lookup(module, lookup_none, nullptr).has_value(),
                        "repeating an identical symbol lookup configuration must be idempotent");
    test_support::check(INB::Runtime::set_symbol_lookup(module, lookup_partial, nullptr).has_value(),
                        "changing the resolver before the runtime is ready must still succeed");

    /// core-ready 不要求 invoke/unbox/类型识别等非核心导出，legacy-ready 仍按原 required 集合判断。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fake_il2cpp::disable_symbol("il2cpp_runtime_invoke");
        fake_il2cpp::disable_symbol("il2cpp_object_unbox");
        fake_il2cpp::disable_symbol("il2cpp_type_get_type");

        const auto context = make_resolved_context();

        const auto core = INB::detail::refresh_core(context);
        test_support::check(core.has_value(), "core readiness must not require invocation exports");
        test_support::check(context->core.state == INB::RuntimeState::ready,
                            "core readiness must publish the ready state");
        test_support::check(group_missing(*context, "type"),
                            "missing type exports must be reported under the type capability group");
        test_support::check(std::find(context->missing_by_group.at("type").begin(),
                                      context->missing_by_group.at("type").end(),
                                      "il2cpp_type_get_type") != context->missing_by_group.at("type").end(),
                            "the missing type export must be named in its capability group");

        const auto legacy = INB::detail::refresh_context(context);
        test_support::check(!legacy.has_value() && legacy.error().code == INB::ErrorCode::unsupported_runtime,
                            "legacy readiness must still require the original export set");

        fake_il2cpp::clear_disabled_symbols();
    }

    /// 缺失能力只影响依赖它的操作：屏蔽字符串导出不影响 core 就绪，只进入 string 分组。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fake_il2cpp::disable_symbol("il2cpp_string_chars");

        const auto context = make_resolved_context();
        const auto core = INB::detail::refresh_core(context);
        test_support::check(core.has_value(), "a missing string export must not block core readiness");
        test_support::check(group_missing(*context, "string"), "the missing string export must be grouped as string");
        test_support::check(std::find(context->core.capabilities.begin(), context->core.capabilities.end(), "string") ==
                                    context->core.capabilities.end(),
                            "a capability group with a missing export must not be advertised");
        test_support::check(std::find(context->core.capabilities.begin(), context->core.capabilities.end(), "readiness") !=
                                    context->core.capabilities.end(),
                            "core readiness itself must stay advertised");
        test_support::check(context->snapshot.missing_apis.empty(),
                            "optional exports must never enter the required-missing list");

        fake_il2cpp::clear_disabled_symbols();
    }

    /// legacy-ready 先建立 generation，core-ready 不得再次提升；反序亦然。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();

        const auto legacy_first = make_resolved_context();
        test_support::check(INB::detail::refresh_context(legacy_first).has_value(),
                            "legacy readiness must succeed with the complete export table");
        const auto generation = INB::detail::generation_for(legacy_first);
        test_support::check(generation != 0, "the first ready transition must establish a non-zero generation");
        test_support::check(INB::detail::refresh_core(legacy_first).has_value(),
                            "core readiness must succeed after legacy readiness");
        test_support::check(INB::detail::generation_for(legacy_first) == generation,
                            "legacy ready must not bump the generation again");

        const auto core_first = make_resolved_context();
        test_support::check(INB::detail::refresh_core(core_first).has_value(),
                            "core readiness must succeed on its own");
        const auto core_generation = INB::detail::generation_for(core_first);
        test_support::check(core_generation != 0, "core readiness must establish a non-zero generation");
        test_support::check(INB::detail::refresh_context(core_first).has_value(),
                            "legacy readiness must succeed after core readiness");
        test_support::check(INB::detail::generation_for(core_first) == core_generation,
                            "core ready must not bump the generation when legacy becomes ready");
    }

    /// 默认发现要求 get_corlib 可用；导出缺失是 unsupported，未初始化是 runtime_not_ready。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fake_il2cpp::disable_symbol("il2cpp_get_corlib");

        const auto context = make_resolved_context();
        const auto core = INB::detail::refresh_core(context);
        test_support::check(!core.has_value() && core.error().code == INB::ErrorCode::unsupported_runtime,
                            "a runtime without il2cpp_get_corlib must be unsupported for the facade");
        test_support::check(context->core.state == INB::RuntimeState::unsupported,
                            "the core snapshot must record the unsupported state");

        fake_il2cpp::clear_disabled_symbols();
    }

    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_corlib_available(false);

        const auto context = make_resolved_context();
        const auto core = INB::detail::refresh_core(context);
        test_support::check(!core.has_value() && core.error().code == INB::ErrorCode::runtime_not_ready,
                            "a null corlib must keep the facade waiting instead of failing unsupported");
        test_support::check(context->core.state == INB::RuntimeState::initializing,
                            "the core snapshot must stay initializing while corlib is null");

        fixture.set_corlib_available(true);
        test_support::check(INB::detail::refresh_core(context).has_value(),
                            "core readiness must recover once the runtime initializes");
    }

    /// 程序集尚未可见时 core 仍未就绪，可见后转为 ready。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_assemblies_available(false);

        const auto context = make_resolved_context();
        const auto core = INB::detail::refresh_core(context);
        test_support::check(!core.has_value() && core.error().code == INB::ErrorCode::runtime_not_ready,
                            "core readiness must wait for the first loaded assemblies");
        test_support::check(context->core.state == INB::RuntimeState::initializing,
                            "the core snapshot must report initializing while assemblies are not visible");

        fixture.set_assemblies_available(true);
        test_support::check(INB::detail::refresh_core(context).has_value(),
                            "core readiness must succeed once assemblies become visible");
    }
}
