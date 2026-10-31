// OFF 构建（不带 Frida Gum 后端）的独立消费者：查询与调用必须照常工作，
// 替换必须按 missing_api 失败，而不是在链接期出现未定义符号。
#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>

using namespace il2cpp_native_bridge;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

__attribute__((noinline)) std::int32_t off_entry(std::int32_t value) { return value + 1; }

struct OffFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};

    OffFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        auto* take = fixture.add_method(counter, "Take", "System.Int32", {"System.Int32"}, true,
                                        [this](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                                            return fixture.box(args[0], sizeof(int));
                                        });
        fixture.set_method_entry(take, reinterpret_cast<void*>(&off_entry));
    }
};

std::int32_t on_take(ReplacementCall<std::int32_t, std::int32_t>&, std::int32_t value) { return value; }

template <typename Callback>
Outcome run_method(const char* name, std::size_t parameter_count, Callback&& callback) {
    return run_in_scope([&](Outcome& outcome) {
        auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                              .method(name, parameter_count);
        callback(method, outcome);
    });
}

} /// namespace

int main() {
    try {
        OffFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());

        const auto resolved = run_method("Take", 1, [](const Method&, Outcome& outcome) {
            outcome.name = std::string(Il2Cpp::domain().assembly("AssemblyA").name());
        });
        test_support::check(resolved.ok && resolved.name == "AssemblyA",
                            "metadata lookup must work without the Gum backend");

        const auto invoked = run_method("Take", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = method.invoke<std::int32_t, std::int32_t>(9);
        });
        test_support::check(invoked.ok && invoked.int_value == 9,
                            "invoke must work without the Gum backend");

        const auto installed = run_method("Take", 1, [](const Method& method, Outcome&) {
            method.implementation(&on_take);
        });
        test_support::check(!installed.ok && installed.code == ErrorCode::missing_api,
                            "replacement must fail with missing_api when the backend is off");
        test_support::check(installed.stage == ErrorStage::replacement,
                            "the backend failure must be reported at the replacement stage");

        const auto state = run_method("Take", 1, [](const Method& method, Outcome& outcome) {
            outcome.int_value = static_cast<int>(method.replacement_state());
        });
        test_support::check(state.ok && state.int_value == static_cast<int>(ReplacementState::not_installed),
                            "an unavailable backend must project not_installed");

        const auto reverted = run_method("Take", 1, [](const Method& method, Outcome&) { method.revert(); });
        test_support::check(reverted.ok, "revert without a managed record must stay a no-op success");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "off-provider checks failed: %s\n", error.what());
        return 1;
    }
    std::fputs("off-provider checks passed\n", stdout);
    return 0;
}
