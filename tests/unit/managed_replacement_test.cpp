#include "../frida_gum_stub/fake_frida_gum_control.hpp"
#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace INB;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

/// 目标入口只需可执行；替身会直接调用替换入口，不会真的跳到这些函数。
__attribute__((noinline)) std::int32_t entry_add(std::int32_t value) { return value + 1; }
__attribute__((noinline)) std::int32_t entry_read(Il2CppObject* self) { return self != nullptr ? 42 : 0; }

/// 原函数跳板：签名必须与真实 ABI 一致（隐藏 MethodInfo 在末位）。
std::int32_t original_add(std::int32_t value, const void* method) {
    (void) method;
    return value + 1;
}

std::int32_t original_read(Il2CppObject* self, const void* method) {
    (void) method;
    return self != nullptr ? 42 : 0;
}

int g_callback_calls{};
int g_original_value{};
Il2CppObject* g_self_seen{};
std::string g_scope_assembly;

/// 安装钩子内观察到的恢复结果；钩子是普通函数指针，只能通过全局传递。
const Method* g_install_target{};
bool g_install_revert_called{};
ErrorCode g_install_revert_code{ErrorCode::internal_error};

void on_install_revert(void*, void*, void*) {
    g_install_revert_called = true;
    try {
        g_install_target->revert();
    } catch (const BridgeError& error) {
        g_install_revert_code = error.error().code;
    }
}

std::int32_t on_add(ReplacementCall<std::int32_t, std::int32_t>& call, std::int32_t value) {
    ++g_callback_calls;
    /// 回调内应当已经建立访问作用域：链式查询可直接使用。
    g_scope_assembly = std::string(Il2Cpp::domain().assembly("AssemblyA").name());
    g_original_value = call.original(value);
    return value + 100;
}

std::int32_t on_read(ReplacementCall<std::int32_t>& call) {
    ++g_callback_calls;
    g_self_seen = call.self();
    return 7;
}

std::int32_t on_throw(ReplacementCall<std::int32_t, std::int32_t>&, std::int32_t) {
    ++g_callback_calls;
    throw std::runtime_error("callback exploded");
}

/// 固定的探针运行时。
struct ReplacementFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};
    Il2CppObject* instance{};

    ReplacementFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        instance = fixture.make_object(counter);
    }

    Il2CppMethodInfo* add_method(const char* name, bool is_static, void* entry, std::vector<std::string> parameters) {
        const std::string return_type = "System.Int32";
        auto* method = fixture.add_method(counter, name, return_type, std::move(parameters), is_static);
        fixture.set_method_entry(method, entry);
        return method;
    }
};

/// 在作用域内解析方法并执行回调。
template <typename Callback>
Outcome run_method(const char* name, std::size_t parameter_count, Callback&& callback) {
    return run_in_scope([&](Outcome& outcome) {
        auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                              .method(name, parameter_count);
        callback(method, outcome);
    });
}

/// 预算用例需要互不相同的可执行地址；模板实例化保证每个槽位一个独立函数。
template <int Index>
__attribute__((noinline)) std::int32_t slot_entry(std::int32_t value) { return value + Index; }

template <std::size_t... Index>
std::array<void*, sizeof...(Index)> slot_entries(std::index_sequence<Index...>) {
    return {reinterpret_cast<void*>(&slot_entry<static_cast<int>(Index)>)...};
}

} /// namespace

void run_managed_replacement_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 安装后回调生效、original 转发、state 为 active。
    {
        ReplacementFixture context;
        auto* method = context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_add), reinterpret_cast<void*>(&original_add));
        test_support::FacadeHarness harness(context.fixture.api());
        g_callback_calls = 0;
        g_original_value = 0;

        const auto installed = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(installed.ok, "installing a replacement must succeed");

        const auto state = run_method("Add", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = static_cast<int>(method.replacement_state());
        });
        test_support::check(state.ok && state.int_value == static_cast<int>(ReplacementState::active),
                            "an installed replacement must report active");

        const auto called = fake_frida_gum::invoke<std::int32_t, std::int32_t, const void*>(
                reinterpret_cast<void*>(&entry_add), 5, nullptr);
        test_support::check(called == 105, "the replacement callback must run and override the result");
        test_support::check(g_callback_calls == 1, "the callback must run exactly once per call");
        test_support::check(g_original_value == 6, "original() must forward to the trampoline");
        test_support::check(g_scope_assembly == "AssemblyA",
                            "the callback must run inside a runtime access scope");

        const auto cleaned = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(cleaned.ok, "the replacement must be restorable at the end of the case");
        (void) method;
    }

    /// 实例方法：self 可见，original 保留实例。
    {
        ReplacementFixture context;
        context.add_method("Read", false, reinterpret_cast<void*>(&entry_read), {});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_read), reinterpret_cast<void*>(&original_read));
        test_support::FacadeHarness harness(context.fixture.api());
        g_callback_calls = 0;
        g_self_seen = nullptr;

        const auto installed = run_method("Read", 0, [](const Method& method, Outcome&) {
            method.implementation(&on_read);
        });
        test_support::check(installed.ok, "installing on an instance method must succeed");

        const auto called = fake_frida_gum::invoke<std::int32_t, Il2CppObject*, const void*>(
                reinterpret_cast<void*>(&entry_read), context.instance, nullptr);
        test_support::check(called == 7, "the instance replacement must run");
        test_support::check(g_self_seen == context.instance, "self() must expose the current instance");

        const auto cleaned = run_method("Read", 0, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(cleaned.ok, "the instance replacement must be restorable");
    }

    /// 重复安装被拒绝；恢复后回到原实现且可重装。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_add), reinterpret_cast<void*>(&original_add));
        test_support::FacadeHarness harness(context.fixture.api());

        const auto first = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(first.ok, "the first install must succeed");

        const auto duplicate = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(!duplicate.ok && duplicate.code == ErrorCode::owner_conflict,
                            "a second install on the same target must be rejected");

        const auto reverted = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(reverted.ok, "revert must succeed when the barrier is clear");

        const auto restored = run_method("Add", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = static_cast<int>(method.replacement_state());
        });
        test_support::check(restored.int_value == static_cast<int>(ReplacementState::not_installed),
                            "a successful revert must project not_installed");
        test_support::check(fake_frida_gum::invoke<std::int32_t, std::int32_t, const void*>(
                                    reinterpret_cast<void*>(&entry_add), 5, nullptr) == 0,
                            "after revert the stub no longer has a replacement to run");

        const auto again = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(again.ok, "reinstalling after a confirmed revert must succeed");
    }

    /// 没有 managed 项时重复恢复成功。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api());

        const auto reverted = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(reverted.ok, "revert without an installation must be a no-op success");
    }

    /// flush 未确认时进入 draining 并抛 restore_pending，随后可重试成功。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_add), reinterpret_cast<void*>(&original_add));
        test_support::FacadeHarness harness(context.fixture.api());

        const auto installed = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(installed.ok, "install must succeed before draining");

        fake_frida_gum::set_in_flight(true);
        const auto draining = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(!draining.ok && draining.code == ErrorCode::restore_pending,
                            "an unconfirmed flush must report restore_pending");

        const auto state = run_method("Add", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = static_cast<int>(method.replacement_state());
        });
        test_support::check(state.int_value == static_cast<int>(ReplacementState::draining),
                            "an unconfirmed flush must leave the record draining");

        fake_frida_gum::set_in_flight(false);
        const auto retried = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(retried.ok, "retrying revert after the barrier clears must succeed");
    }

    /// 用户回调抛异常被隔离：调用返回默认值，不穿越 native 栈。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_add), reinterpret_cast<void*>(&original_add));
        test_support::FacadeHarness harness(context.fixture.api());
        g_callback_calls = 0;

        const auto installed = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_throw);
        });
        test_support::check(installed.ok, "installing a throwing callback must succeed");

        const auto called = fake_frida_gum::invoke<std::int32_t, std::int32_t, const void*>(
                reinterpret_cast<void*>(&entry_add), 5, nullptr);
        test_support::check(called == 0, "a throwing callback must yield the default result");
        test_support::check(g_callback_calls == 1, "the throwing callback must run exactly once");
        test_support::check(diagnostics.contains("category=callback"),
                            "a throwing callback must be reported as a diagnostic");

        const auto cleaned = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(cleaned.ok, "the throwing replacement must be restorable");
    }

    /// 连续 100 次调用都命中替换，且回调计数与返回值一致。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        fake_frida_gum::set_original(reinterpret_cast<void*>(&entry_add), reinterpret_cast<void*>(&original_add));
        test_support::FacadeHarness harness(context.fixture.api());
        g_callback_calls = 0;

        const auto installed = run_method("Add", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(installed.ok, "install must succeed before the callback count check");

        int mismatched = 0;
        for (int index = 0; index < 100; ++index) {
            const auto called = fake_frida_gum::invoke<std::int32_t, std::int32_t, const void*>(
                    reinterpret_cast<void*>(&entry_add), index, nullptr);
            if (called != index + 100) ++mismatched;
        }
        test_support::check(mismatched == 0, "all 100 calls must return the replacement result");
        test_support::check(g_callback_calls == 100, "the callback must run exactly 100 times");

        const auto cleaned = run_method("Add", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(cleaned.ok, "the replacement must be restorable after 100 calls");
    }

    /// 安装返回前的回调里发起恢复：请求被记录，安装完成后立即撤销。
    {
        ReplacementFixture context;
        context.add_method("Add", true, reinterpret_cast<void*>(&entry_add), {"System.Int32"});
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api());
        g_install_target = nullptr;
        g_install_revert_called = false;
        g_install_revert_code = ErrorCode::internal_error;
        fake_frida_gum::set_install_hook(&on_install_revert);

        const auto installed = run_in_scope([&](Outcome&) {
            auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                  .method("Add", 1);
            g_install_target = &method;
            method.implementation(&on_add);
        });
        fake_frida_gum::set_install_hook(nullptr);
        g_install_target = nullptr;

        test_support::check(installed.ok, "an install with a deferred restore must still return");
        test_support::check(g_install_revert_called,
                            "the install hook must observe the record while it is installing");
        test_support::check(g_install_revert_code == ErrorCode::restore_pending,
                            "a restore during install must report restore_pending");

        const auto state = run_method("Add", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = static_cast<int>(method.replacement_state());
        });
        test_support::check(state.int_value == static_cast<int>(ReplacementState::not_installed),
                            "a deferred restore must leave nothing installed");
        test_support::check(fake_frida_gum::replacement_for(reinterpret_cast<void*>(&entry_add)) == nullptr,
                            "a deferred restore must revert the target in Gum");
    }

    /// 预算：256 个名额用满后拒绝，释放一个即可继续安装。
    {
        ReplacementFixture context;
        constexpr std::size_t kSlots = 256;
        const auto entries = slot_entries(std::make_index_sequence<kSlots + 1>{});
        std::vector<std::string> names;
        names.reserve(kSlots + 1);
        for (std::size_t index = 0; index < kSlots + 1; ++index) {
            names.push_back("Slot" + std::to_string(index));
            auto* method = context.add_method(names.back().c_str(), true, entries[index], {"System.Int32"});
            test_support::check(method != nullptr, "the fixture must expose every budget slot");
        }

        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api());

        std::size_t installed_count = 0;
        for (std::size_t index = 0; index < kSlots; ++index) {
            const auto installed = run_method(names[index].c_str(), 1, [](const Method& method, Outcome&) {
                method.implementation(&on_add);
            });
            test_support::check(installed.ok, "every slot within the budget must install");
            ++installed_count;
        }

        const auto over_budget = run_method(names[kSlots].c_str(), 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(!over_budget.ok && over_budget.code == ErrorCode::resource_exhausted,
                            "the 257th install must hit the budget limit");

        const auto released = run_method(names[0].c_str(), 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(released.ok, "releasing one slot must succeed");

        const auto reinstalled = run_method(names[kSlots].c_str(), 1, [](const Method& method, Outcome&) {
            method.implementation(&on_add);
        });
        test_support::check(reinstalled.ok, "a released slot must be reusable");

        /// 注册表是进程级的：必须全部回收，否则后续分组会撞预算。
        for (std::size_t index = 1; index < kSlots; ++index) {
            const auto reverted = run_method(names[index].c_str(), 1, [](const Method& method, Outcome&) {
                method.revert();
            });
            test_support::check(reverted.ok, "every installed slot must be restorable");
        }
        const auto last = run_method(names[kSlots].c_str(), 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(last.ok, "the reused slot must be restorable");
        test_support::check(installed_count == kSlots, "the budget case must install exactly 256 replacements");
    }
}
