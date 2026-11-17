#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/runtime_internal.hpp"

#include <string>

using namespace INB;

namespace {

/// 无程序集限定的查询必须命中缓存，且两条路径返回同一个程序集名。
void check_unscoped_lookup_is_cached() {
    fake_il2cpp::Fixture fixture;
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());

    fixture.reset_class_lookup_count();
    auto first = runtime.klass("UnityEngine.Time");
    const auto first_lookups = fixture.class_lookup_count();
    auto second = runtime.klass("UnityEngine.Time");
    const auto second_lookups = fixture.class_lookup_count() - first_lookups;

    test_support::check(first.found() && second.found(), "the same class must resolve twice");
    test_support::check(first_lookups > 0, "the first lookup must scan the assemblies");
    test_support::check(second_lookups == 0, "an unscoped repeat lookup must hit the cache");
    test_support::check(first.assembly_name() == second.assembly_name(),
                        "cache hits and misses must report the same assembly name");
    test_support::check(!first.assembly_name().empty(), "a resolved class must report its assembly");
}

/// in_assembly 在程序集已匹配时不应触发第二次全量扫描。
void check_in_assembly_short_circuits() {
    fake_il2cpp::Fixture fixture;
    fixture.add_class("ProbeAssembly", "Probe", "Sample");
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());

    fixture.reset_class_lookup_count();
    auto resolved = runtime.klass("Probe.Sample");
    const auto after_resolve = fixture.class_lookup_count();
    auto scoped = resolved.in_assembly("ProbeAssembly");
    const auto after_scoped = fixture.class_lookup_count();

    test_support::check(resolved.found(), "the probe class must resolve");
    test_support::check(after_resolve > 0, "resolving the probe class must scan the assemblies");
    test_support::check(scoped.found(), "narrowing to the matching assembly must keep the class");
    test_support::check(after_scoped == after_resolve, "in_assembly must not rescan when the assembly already matches");
    test_support::check(scoped.assembly_name() == resolved.assembly_name(),
                        "in_assembly must not rewrite the resolved assembly name");

    /// 带 .dll 后缀的请求同样应被视为匹配。
    fixture.reset_class_lookup_count();
    auto suffixed = runtime.klass("Probe.Sample").in_assembly("ProbeAssembly.dll");
    test_support::check(suffixed.found(), "a .dll suffix must still match the assembly");
    test_support::check(fixture.class_lookup_count() == 0, "a matching .dll suffix must not rescan");
}

/// 跨程序集重名仍要报歧义，但限定程序集后必须解析成功。
void check_duplicate_class_still_diagnosed() {
    fake_il2cpp::Fixture fixture;
    fixture.add_class("DuplicateA", "Probe", "Duplicate");
    fixture.add_class("DuplicateB", "Probe", "Duplicate");
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());

    auto ambiguous = runtime.klass("Probe.Duplicate");
    test_support::check(!ambiguous.found() && ambiguous.error().code == ErrorCode::ambiguous_match,
                        "a class duplicated across assemblies must stay ambiguous");

    auto narrowed = runtime.klass("Probe.Duplicate").in_assembly("DuplicateB");
    test_support::check(narrowed.found(), "narrowing by assembly must resolve a duplicated class");
    test_support::check(narrowed.assembly_name() == "DuplicateB",
                        "the narrowed result must report the image name");
}

/// 缓存超过上限后整体清空，后续查询仍必须正确解析。
void check_cache_limit_recovers() {
    fake_il2cpp::Fixture fixture;
    fixture.add_class("AnchorAssembly", "Probe", "Anchor");
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());

    test_support::check(runtime.klass("Probe.Anchor").found(), "the anchor class must resolve");
    fixture.reset_class_lookup_count();
    test_support::check(runtime.klass("Probe.Anchor").found(), "the cached anchor class must resolve");
    test_support::check(fixture.class_lookup_count() == 0, "the anchor class must come from the cache");

    for (std::size_t index = 0; index <= detail::kClassCacheLimit; ++index) {
        const auto name = "Type" + std::to_string(index);
        fixture.add_class("BulkAssembly", "Bulk", name);
        (void) runtime.klass("Bulk." + name);
    }

    fixture.reset_class_lookup_count();
    auto recovered = runtime.klass("Probe.Anchor");
    test_support::check(recovered.found(), "a class must still resolve after the cache is cleared");
    test_support::check(fixture.class_lookup_count() > 0,
                        "exceeding the cache limit must drop the old entries");
}

} /// namespace

void run_class_cache_tests() {
    check_unscoped_lookup_is_cached();
    check_in_assembly_short_circuits();
    check_duplicate_class_still_diagnosed();
    check_cache_limit_recovers();
}
