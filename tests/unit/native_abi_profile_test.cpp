#include "../frida_gum_stub/fake_frida_gum_control.hpp"
#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "hooks/abi_profile.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstdint>
#include <optional>
#include <string>

using namespace INB;
using detail::ProfileSupport;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

/// bootstrap 结果；失败时只带错误码，成功时带完整 profile。
struct ProfileOutcome {
    bool ok{};
    ErrorCode code{ErrorCode::internal_error};
    detail::AbiProfile profile;
};

ProfileOutcome read_profile() {
    ProfileOutcome result;
    const auto outcome = run_in_scope([&](Outcome&) {
        auto profile = detail::resolve_abi_profile(detail::require_access_scope());
        if (!profile) throw BridgeError(profile.error());
        result.profile = profile.value();
        result.ok = true;
    });
    if (!result.ok) result.code = outcome.code;
    return result;
}

/// 只搭一个带版本的运行时，用于 bootstrap 用例。
struct ProfileFixture {
    fake_il2cpp::Fixture fixture;

    explicit ProfileFixture(std::optional<std::u16string> version = std::u16string{u"6000.3.10f1"}) {
        fixture.add_corlib_types();
        fixture.set_unity_version(std::move(version));
    }
};

/// 目标入口只需可执行；替身会直接调用替换入口，不会真的跳到这个函数。
__attribute__((noinline)) std::int32_t profile_entry_add(std::int32_t value) { return value + 1; }

/// 带一个可替换目标方法的运行时，用于安装关口用例。
struct InstallFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};

    InstallFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        auto* method = fixture.add_method(counter, "Add", "System.Int32", {"System.Int32"}, true);
        fixture.set_method_entry(method, reinterpret_cast<void*>(&profile_entry_add));
    }
};

std::int32_t on_add(ReplacementCall<std::int32_t, std::int32_t>&, std::int32_t value) { return value + 100; }

Outcome install_add() {
    return run_in_scope([&](Outcome&) {
        Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                .method("Add", 1)
                .implementation(&on_add);
    });
}

} /// namespace

void run_native_abi_profile_tests() {
    /// 现代 family：形态与证据等级；candidate 不等于获准支持。
    {
        ProfileFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(profile.ok, "bootstrap must succeed on a modern runtime");
        test_support::check(profile.profile.family == "modern-static",
                            "Unity 6000.3 must map to the modern-static family");
        test_support::check(profile.profile.unity_version == "6000.3.10f1",
                            "the profile must carry the version read from the runtime");
        test_support::check(profile.profile.entry_offset == 0,
                            "the modern entry offset must come from the generated-code evidence");
        test_support::check(!profile.profile.dummy_instance_static,
                            "a modern static method has no dummy instance slot");
        test_support::check(profile.profile.hidden_method_info,
                            "a modern native entry carries the hidden MethodInfo");
        test_support::check(profile.profile.support == ProfileSupport::approved,
                            "the runtime-verified version must be approved");
    }

    /// 版本分界：2018.3 起为 modern，2018.2 及以前为 legacy。
    {
        const struct {
            std::u16string version;
            const char* family;
        } cases[] = {
                {u"2018.3.0f2", "modern-static"},
                {u"2021.3.39f1", "modern-static"},
                {u"2018.2.21f1", "legacy-static-slot"},
                {u"2017.4.40f1", "legacy-static-slot"},
                {u"5.3.8f1", "legacy-static-slot"},
        };
        for (const auto& item : cases) {
            ProfileFixture context{item.version};
            test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
            const auto profile = read_profile();
            test_support::check(profile.ok, "bootstrap must map every known version range");
            test_support::check(profile.profile.family == item.family,
                                "the family must follow the 2018.3 boundary");
        }
    }

    /// 同 family 但未验证的版本只到 candidate，不声称支持。
    {
        ProfileFixture context{u"2021.2.0f1"};
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(profile.ok, "an unverified modern version must still resolve");
        test_support::check(profile.profile.family == "modern-static",
                            "an unverified modern version keeps the modern family");
        test_support::check(profile.profile.support == ProfileSupport::candidate,
                            "an unverified version must stay candidate even inside a verified family");
    }

    /// 目标应用实测的两个版本与 6000.3 一样进 approved。
    {
        const std::u16string verified[] = {u"2021.3.39f1", u"2022.3.62f2"};
        for (const auto& version : verified) {
            ProfileFixture context{version};
            test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
            const auto profile = read_profile();
            test_support::check(profile.ok, "a device-verified version must resolve");
            test_support::check(profile.profile.support == ProfileSupport::approved,
                                "a device-verified version must be approved");
        }
    }

    /// legacy family 只有假设形态，没有生成源码证据：必须拒绝。
    {
        ProfileFixture context{u"2018.2.21f1"};
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(profile.ok, "the legacy family must still be recognised");
        test_support::check(profile.profile.dummy_instance_static,
                            "the legacy static slot hypothesis must stay visible");
        test_support::check(profile.profile.support == ProfileSupport::rejected,
                            "an unverified family must be rejected");
    }

    /// 没有 family 覆盖的版本：按未知运行时拒绝。
    {
        for (const auto& version : {std::u16string{u"4.7.2f1"}, std::u16string{u"banana"}}) {
            ProfileFixture context{version};
            test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
            const auto profile = read_profile();
            test_support::check(!profile.ok && profile.code == ErrorCode::unsupported_runtime,
                                "an unknown version must map to unsupported_runtime");
        }
    }

    /// getter 缺失。
    {
        ProfileFixture context;
        context.fixture.remove_unity_version_method();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(!profile.ok && profile.code == ErrorCode::not_found,
                            "a missing version getter must map to not_found");
    }

    /// 形态不符的候选不算 getter：带参数、非 static 都必须被跳过。
    {
        ProfileFixture context;
        context.fixture.remove_unity_version_method();
        auto* application = context.fixture.find_class("UnityEngine.Application");
        test_support::check(application != nullptr, "the fixture must expose UnityEngine.Application");
        context.fixture.add_method(application, "get_unityVersion", "System.String", {"System.Int32"}, true);
        context.fixture.add_method(application, "get_unityVersion", "System.String", {}, false);
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(!profile.ok && profile.code == ErrorCode::not_found,
                            "a getter with the wrong static/parameter shape must be skipped");
    }

    /// 托管异常优先于空结果。
    {
        ProfileFixture context;
        context.fixture.set_unity_version_exception(true);
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(!profile.ok && profile.code == ErrorCode::managed_exception,
                            "a throwing version getter must map to managed_exception");
    }

    /// 无异常但返回 null。
    {
        ProfileFixture context{std::nullopt};
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto profile = read_profile();
        test_support::check(!profile.ok && profile.code == ErrorCode::invocation_failed,
                            "a null version string must map to invocation_failed");
    }

    /// 字符串导出缺失。
    {
        ProfileFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        harness.context()->api.string_chars = nullptr;
        const auto profile = read_profile();
        test_support::check(!profile.ok && profile.code == ErrorCode::missing_api,
                            "a missing string export must map to missing_api");
    }

    /// 安装关口：未获证的 family 在进入 Gum 之前被拒绝。
    {
        InstallFixture context;
        context.fixture.set_unity_version(std::u16string{u"2018.2.21f1"});
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto installed = install_add();
        test_support::check(!installed.ok && installed.code == ErrorCode::unsupported_runtime,
                            "installing on an unverified family must be refused");
        test_support::check(fake_frida_gum::replacement_for(reinterpret_cast<void*>(&profile_entry_add)) == nullptr,
                            "a refused install must not reach the Gum interceptor");
    }

    /// 现代 family 在候选等级下允许安装（安装返回前已通过 profile 关口）。
    {
        InstallFixture context;
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto installed = install_add();
        test_support::check(installed.ok, "a candidate profile must allow installation");
        test_support::check(fake_frida_gum::replacement_for(reinterpret_cast<void*>(&profile_entry_add)) != nullptr,
                            "the candidate install must reach the Gum interceptor");
        const auto reverted = run_in_scope([&](Outcome&) {
            Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                    .method("Add", 1)
                    .revert();
        });
        test_support::check(reverted.ok, "the candidate install must be restorable");
    }

    /// 泛型目标在绑定期就被拒绝，不依赖 profile。
    {
        InstallFixture context;
        auto* generic = context.fixture.add_method(context.counter, "Map", "System.Int32", {"System.Int32"}, true,
                                                   {}, true);
        context.fixture.set_method_entry(generic, reinterpret_cast<void*>(&profile_entry_add));
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        const auto installed = run_in_scope([&](Outcome&) {
            Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                    .method("Map", 1)
                    .implementation(&on_add);
        });
        test_support::check(!installed.ok && installed.code == ErrorCode::unsupported_runtime,
                            "a generic method must be refused before installation");
    }
}
