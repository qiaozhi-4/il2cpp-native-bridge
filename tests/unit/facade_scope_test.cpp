#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace INB;

namespace {

/// 当前线程是否已附加到 fake runtime；作用域边界用它观察租约归属。
bool thread_attached(const detail::Api& api) { return api.thread_current() != nullptr; }

} /// namespace

void run_facade_scope_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 作用域之外执行运行时操作必须抛 scope_violation，且不隐式附加线程。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        bool thrown = false;
        try {
            (void) detail::require_access_scope();
        } catch (const BridgeError& error) {
            thrown = error.error().code == ErrorCode::scope_violation && error.error().stage == ErrorStage::scope;
        }
        test_support::check(thrown, "runtime operations outside a perform scope must raise a scope violation");
        test_support::check(!thread_attached(fixture.api()), "a rejected scope must not attach the calling thread");
    }

    /// 本次附加：作用域内线程已附加，作用域结束只分离本次取得的租约。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());
        test_support::check(detail::refresh_core(harness.context()).has_value(), "the harness runtime must become core ready");

        const auto api = fixture.api();
        bool attached_inside = false;
        {
            detail::AccessScope scope(harness.context());
            attached_inside = thread_attached(api);
            test_support::check(&detail::require_access_scope() == &scope.view(),
                                "the active scope must expose its ready view");
        }
        test_support::check(attached_inside, "the scope must attach the calling thread");
        test_support::check(!thread_attached(api), "the scope must detach the thread it attached");
    }

    /// 预附加：作用域不接管已有附加状态，结束后线程仍保持附加。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());
        test_support::check(detail::refresh_core(harness.context()).has_value(), "the harness runtime must become core ready");

        const auto api = fixture.api();
        bool attached_inside = false;
        bool attached_after = false;
        std::thread worker([&] {
            api.thread_attach(fixture.domain());
            {
                detail::AccessScope scope(harness.context());
                attached_inside = thread_attached(api);
            }
            attached_after = thread_attached(api);
            api.thread_detach(api.thread_current());
        });
        worker.join();

        test_support::check(attached_inside, "a pre-attached thread must stay attached inside the scope");
        test_support::check(attached_after, "a scope must not detach a thread it did not attach");
    }

    /// 嵌套作用域复用外层租约；不同 context 在同一线程被拒绝。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());
        test_support::check(detail::refresh_core(harness.context()).has_value(), "the harness runtime must become core ready");

        auto other = detail::make_testing_context(fixture.api(), fixture.domain());
        other->injected_api = true;
        test_support::check(detail::refresh_core(other).has_value(), "the second context must become core ready");

        const auto api = fixture.api();
        {
            detail::AccessScope outer(harness.context());
            {
                detail::AccessScope inner(harness.context());
                test_support::check(&detail::require_access_scope() == &inner.view(),
                                    "the innermost scope must be the active view");
                test_support::check(thread_attached(api), "a nested scope must keep the thread attached");
            }
            test_support::check(thread_attached(api), "leaving a nested scope must not detach the outer lease");
        }
        test_support::check(!thread_attached(api), "leaving the outer scope must detach the thread");

        {
            detail::AccessScope outer(harness.context());
            bool rejected = false;
            try {
                detail::AccessScope conflicting(other);
            } catch (const BridgeError& error) {
                rejected = error.error().code == ErrorCode::scope_violation;
            }
            test_support::check(rejected, "a different runtime context must be rejected inside an active scope");
        }
    }

    /// perform 的 action 在作用域内执行，completion 在租约释放后执行。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        const auto api = fixture.api();
        std::atomic<bool> attached_in_action{false};
        std::atomic<bool> attached_in_completion{true};
        std::atomic<bool> completed{false};
        auto submitted = Il2Cpp::perform(
                [&] {
                    attached_in_action.store(thread_attached(api));
                    (void) detail::require_access_scope();
                },
                [&](Result<void>) {
                    attached_in_completion.store(thread_attached(api));
                    completed.store(true);
                });
        test_support::check(submitted.has_value(), "submitting a scoped action must succeed");
        test_support::check(test_support::wait_until([&] { return completed.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "the scoped action must complete");
        test_support::check(attached_in_action.load(), "the action must run inside an attached scope");
        test_support::check(!attached_in_completion.load(), "completion must run after the lease is released");
    }

    /// nested perform 只排队：内层 action 与 completion 都在外层返回之后发生。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        std::mutex mutex;
        std::vector<std::string> events;
        std::atomic<bool> all_done{false};
        auto submitted = Il2Cpp::perform(
                [&] {
                    { std::lock_guard lock(mutex); events.emplace_back("outer-start"); }
                    auto nested = Il2Cpp::perform(
                            [&] { std::lock_guard lock(mutex); events.emplace_back("inner-action"); },
                            [&](Result<void>) {
                                {
                                    std::lock_guard lock(mutex);
                                    events.emplace_back("inner-done");
                                }
                                all_done.store(true);
                            });
                    test_support::check(nested.has_value(), "a nested perform must be accepted");
                    { std::lock_guard lock(mutex); events.emplace_back("outer-end"); }
                },
                [&](Result<void>) { std::lock_guard lock(mutex); events.emplace_back("outer-done"); });
        test_support::check(submitted.has_value(), "submitting the outer request must succeed");
        test_support::check(test_support::wait_until([&] { return all_done.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "the nested request must eventually run");

        std::lock_guard lock(mutex);
        const std::vector<std::string> expected{"outer-start", "outer-end", "outer-done", "inner-action", "inner-done"};
        test_support::check(events == expected, "a nested perform must only queue behind the running request");
    }

    /// 通知回调抛错被隔离：不影响请求结果，也不影响后续请求。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_corlib_available(false);
        test_support::FacadeHarness harness(fixture.api());

        std::atomic<bool> completed{false};
        PerformOptions options;
        options.diagnostic_after = std::chrono::milliseconds(10);
        options.on_wait = [](const RuntimeSnapshot&, std::chrono::milliseconds) {
            throw std::runtime_error("wait callback exploded");
        };
        auto submitted = Il2Cpp::perform([] {}, [&completed](Result<void>) { completed.store(true); }, options);
        test_support::check(submitted.has_value(), "a throwing wait callback must not reject the submission");

        fixture.set_corlib_available(true);
        test_support::check(test_support::wait_until([&] { return completed.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "the request must still complete when the wait callback throws");

        std::atomic<bool> after{false};
        auto throwing_completion = Il2Cpp::perform(
                [] {}, [](Result<void>) { throw std::runtime_error("completion exploded"); });
        test_support::check(throwing_completion.has_value(), "a throwing completion must not break submission");
        auto follow_up = Il2Cpp::perform([] {}, [&after](Result<void>) { after.store(true); });
        test_support::check(follow_up.has_value(), "the executor must keep accepting work after a completion throws");
        test_support::check(test_support::wait_until([&] { return after.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "a request submitted after a throwing completion must still run");
        test_support::check(diagnostics.contains("category=callback"),
                            "notification callback failures must be reported as diagnostics");
    }

    /// 完成后立即释放 action 捕获，completion 之后不再持有。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        auto token = std::make_shared<int>(7);
        std::weak_ptr<int> observer = token;
        std::atomic<bool> expired_before_completion{false};
        std::atomic<bool> completed{false};
        auto submitted = Il2Cpp::perform(
                [token] { (void) token; },
                [&](Result<void>) {
                    expired_before_completion.store(observer.expired());
                    completed.store(true);
                });
        token.reset();
        test_support::check(submitted.has_value(), "submitting a capturing action must succeed");
        test_support::check(test_support::wait_until([&] { return completed.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "the capturing action must complete");
        test_support::check(expired_before_completion.load(),
                            "the action capture must be released before completion runs");
    }
}
