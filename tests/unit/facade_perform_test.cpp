#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace INB;

namespace {

/// 记录执行顺序、执行线程与完成次数，用于断言 FIFO、单线程与恰好一次。
class Recorder {
public:
    void on_action(int index) {
        std::lock_guard lock(mutex_);
        order_.push_back(index);
        threads_.insert(std::this_thread::get_id());
        ++actions_;
    }

    void on_completion(bool ok) {
        std::lock_guard lock(mutex_);
        ++completions_;
        if (!ok) ++failures_;
        signal_.notify_all();
    }

    [[nodiscard]] bool wait_for_completions(int expected, std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return signal_.wait_for(lock, limit, [&] { return completions_ >= expected; });
    }

    [[nodiscard]] int actions() const { std::lock_guard lock(mutex_); return actions_; }
    [[nodiscard]] int completions() const { std::lock_guard lock(mutex_); return completions_; }
    [[nodiscard]] int failures() const { std::lock_guard lock(mutex_); return failures_; }
    [[nodiscard]] std::vector<int> order() const { std::lock_guard lock(mutex_); return order_; }
    [[nodiscard]] std::size_t thread_count() const { std::lock_guard lock(mutex_); return threads_.size(); }
    [[nodiscard]] bool ran_on(std::thread::id id) const { std::lock_guard lock(mutex_); return threads_.contains(id); }

private:
    mutable std::mutex mutex_;
    std::condition_variable signal_;
    std::vector<int> order_;
    std::set<std::thread::id> threads_;
    int actions_{0};
    int completions_{0};
    int failures_{0};
};

/// 提交 100 个只做计数的请求，供三个加载时机共用。
void submit_hundred(Recorder& recorder) {
    for (int index = 0; index < 100; ++index) {
        auto submitted = Il2Cpp::perform(
                [&recorder, index] { recorder.on_action(index); },
                [&recorder](Result<void> result) { recorder.on_completion(result.has_value()); },
                {});
        test_support::check(submitted.has_value(), "submitting a request must succeed while the queue has room");
    }
}

/// 让 context 回到「模块尚未加载」的状态，模拟冷启动最早时机。
void detach_module(const std::shared_ptr<detail::RuntimeContext>& context) {
    std::lock_guard lock(context->mutex);
    context->injected_api = false;
    context->module = nullptr;
    context->symbol_lookup = nullptr;
    context->core.state = RuntimeState::waiting_for_module;
}

void attach_module(const std::shared_ptr<detail::RuntimeContext>& context) {
    std::lock_guard lock(context->mutex);
    context->module = reinterpret_cast<void*>(0x1);
    context->symbol_lookup = &fake_il2cpp::lookup_symbol;
}

} /// namespace

void run_facade_perform_tests() {
    test_support::DiagnosticCapture diagnostics;

    /// 已就绪：100 次提交各执行一次、各完成一次，只有一个协调线程且不在提交线程执行。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        Recorder recorder;
        const auto caller = std::this_thread::get_id();
        submit_hundred(recorder);

        test_support::check(recorder.wait_for_completions(100, std::chrono::milliseconds(5000)),
                            "all 100 requests must complete on a ready runtime");
        test_support::check(recorder.actions() == 100, "each request must run exactly once");
        test_support::check(recorder.completions() == 100, "each request must report exactly one completion");
        test_support::check(recorder.failures() == 0, "no request may fail on a ready runtime");
        test_support::check(recorder.thread_count() == 1, "one runtime context must use exactly one worker thread");
        test_support::check(!recorder.ran_on(caller), "the action must never run on the submitting thread");

        const auto order = recorder.order();
        test_support::check(order.size() == 100, "the recorder must observe every action");
        for (std::size_t index = 0; index < order.size(); ++index) {
            test_support::check(order[index] == static_cast<int>(index), "requests must run in submission order");
        }
    }

    /// 加载时机一：模块尚未发现时 0 次执行，模块出现后全部执行。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());
        detach_module(harness.context());

        Recorder recorder;
        submit_hundred(recorder);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        test_support::check(recorder.actions() == 0, "no action may run before the runtime module is discovered");
        test_support::check(test_support::core_state(harness.context()) == RuntimeState::waiting_for_module,
                            "the core snapshot must report waiting_for_module");

        attach_module(harness.context());
        test_support::check(recorder.wait_for_completions(100, std::chrono::milliseconds(5000)),
                            "queued requests must run once the module is discovered");
        test_support::check(recorder.actions() == 100, "each queued request must run exactly once");
        test_support::check(recorder.thread_count() == 1, "waiting must not create additional threads");
    }

    /// 加载时机二：模块已加载但运行时未初始化（corlib 为空）时 0 次执行。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_corlib_available(false);
        test_support::FacadeHarness harness(fixture.api());

        Recorder recorder;
        submit_hundred(recorder);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        test_support::check(recorder.actions() == 0, "no action may run while the runtime is still initializing");
        test_support::check(test_support::core_state(harness.context()) == RuntimeState::initializing,
                            "the core snapshot must report initializing");

        fixture.set_corlib_available(true);
        test_support::check(recorder.wait_for_completions(100, std::chrono::milliseconds(5000)),
                            "queued requests must run after the runtime initializes");
        test_support::check(recorder.actions() == 100, "each queued request must run exactly once");
    }

    /// 队列上限：128 个待执行项占满后第 129 个立即拒绝，且不遗留 action/completion。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_corlib_available(false);
        test_support::FacadeHarness harness(fixture.api());

        std::atomic<int> actions{0};
        std::atomic<int> completions{0};
        for (int index = 0; index < 128; ++index) {
            auto submitted = Il2Cpp::perform([&actions] { actions.fetch_add(1, std::memory_order_relaxed); },
                                             [&completions](Result<void>) { completions.fetch_add(1, std::memory_order_relaxed); },
                                             {});
            test_support::check(submitted.has_value(), "the first 128 requests must be accepted");
        }

        std::atomic<int> rejected_actions{0};
        std::atomic<int> rejected_completions{0};
        auto rejected = Il2Cpp::perform([&rejected_actions] { rejected_actions.fetch_add(1, std::memory_order_relaxed); },
                                        [&rejected_completions](Result<void>) { rejected_completions.fetch_add(1, std::memory_order_relaxed); },
                                        {});
        test_support::check(!rejected.has_value(), "the 129th queued request must be rejected");
        test_support::check(rejected.error().code == ErrorCode::resource_exhausted,
                            "a full queue must report resource_exhausted");
        test_support::check(rejected.error().stage == ErrorStage::submit,
                            "a rejected submission must be attributed to the submit stage");

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        test_support::check(rejected_actions.load() == 0, "a rejected request must never run");
        test_support::check(rejected_completions.load() == 0, "a rejected request must never report a completion");

        fixture.set_corlib_available(true);
        test_support::check(test_support::wait_until([&] { return completions.load() >= 128; },
                                                     std::chrono::milliseconds(5000)),
                            "the accepted requests must drain after the runtime becomes ready");
        test_support::check(actions.load() == 128, "exactly the accepted requests may run");

        auto recovered = Il2Cpp::perform([] {});
        test_support::check(recovered.has_value(), "capacity must be released after the queue drains");
    }

    /// 提交期拒绝：空 action 与负等待阈值都在提交阶段失败。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        std::atomic<int> completions{0};
        auto empty = Il2Cpp::perform(std::function<Result<void>()>{},
                                     [&completions](Result<void>) { completions.fetch_add(1); },
                                     {});
        test_support::check(!empty.has_value() && empty.error().code == ErrorCode::invalid_argument,
                            "an empty action must be rejected");

        PerformOptions negative;
        negative.diagnostic_after = std::chrono::milliseconds(-1);
        auto bad_threshold = Il2Cpp::perform([] {}, [&completions](Result<void>) { completions.fetch_add(1); }, negative);
        test_support::check(!bad_threshold.has_value() && bad_threshold.error().code == ErrorCode::invalid_argument,
                            "a negative diagnostic threshold must be rejected");

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        test_support::check(completions.load() == 0, "rejected submissions must not produce completions");

        /// 执行器停机后不再接收新请求：先让执行器真正启动，再停机。
        std::atomic<bool> ran{false};
        auto started = Il2Cpp::perform([&ran] { ran.store(true); });
        test_support::check(started.has_value(), "a valid request must start the executor");
        test_support::check(test_support::wait_until([&] { return ran.load(); }, std::chrono::milliseconds(5000)),
                            "the valid request must run before the executor stops");

        detail::stop_facade_executor(harness.context());
        auto stopped = Il2Cpp::perform([] {});
        test_support::check(!stopped.has_value() && stopped.error().stage == ErrorStage::submit,
                            "a stopped executor must reject new submissions");
    }

    /// 等待诊断：正值阈值每请求最多一次，0 关闭。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        fixture.set_corlib_available(false);
        test_support::FacadeHarness harness(fixture.api());

        std::atomic<int> waits{0};
        PerformOptions options;
        options.diagnostic_after = std::chrono::milliseconds(20);
        options.on_wait = [&waits](const RuntimeSnapshot&, std::chrono::milliseconds) {
            waits.fetch_add(1, std::memory_order_relaxed);
        };

        std::atomic<bool> completed{false};
        auto submitted = Il2Cpp::perform([] {}, [&completed](Result<void>) { completed.store(true); }, options);
        test_support::check(submitted.has_value(), "submitting with a wait diagnostic must succeed");
        test_support::check(test_support::wait_until([&] { return waits.load() >= 1; },
                                                     std::chrono::milliseconds(2000)),
                            "the wait diagnostic must fire once the threshold elapses");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        test_support::check(waits.load() == 1, "the wait diagnostic must fire at most once per request");
        test_support::check(!completed.load(), "the wait threshold must not cancel or complete the request");

        PerformOptions disabled;
        disabled.diagnostic_after = std::chrono::milliseconds(0);
        disabled.on_wait = [&waits](const RuntimeSnapshot&, std::chrono::milliseconds) {
            waits.fetch_add(1, std::memory_order_relaxed);
        };
        auto quiet = Il2Cpp::perform([] {}, {}, disabled);
        test_support::check(quiet.has_value(), "a zero threshold must still accept the request");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        test_support::check(waits.load() == 1, "a zero threshold must disable the wait diagnostic");

        fixture.set_corlib_available(true);
        test_support::check(test_support::wait_until([&] { return completed.load(); },
                                                     std::chrono::milliseconds(5000)),
                            "the request must still complete after the runtime becomes ready");
        test_support::check(PerformOptions{}.diagnostic_after == std::chrono::milliseconds(10000),
                            "the default wait diagnostic threshold must stay at 10 seconds");
    }

    /// action 失败或抛异常只产生一次结果，不重跑。
    {
        fake_il2cpp::Fixture fixture;
        fake_il2cpp::clear_disabled_symbols();
        test_support::FacadeHarness harness(fixture.api());

        std::atomic<int> action_calls{0};
        std::atomic<int> completions{0};
        ErrorCode seen = ErrorCode::internal_error;
        auto failed = Il2Cpp::perform(
                [&action_calls]() -> Result<void> {
                    action_calls.fetch_add(1);
                    return Result<void>::failure(make_error(ErrorCode::invocation_failed, "boom"));
                },
                [&](Result<void> result) {
                    /// 先写结果再发布完成计数，避免调用线程看到完成却读到旧结果。
                    if (!result) seen = result.error().code;
                    completions.fetch_add(1);
                },
                {});
        test_support::check(failed.has_value(), "a failing action must still be accepted");
        test_support::check(test_support::wait_until([&] { return completions.load() == 1; },
                                                     std::chrono::milliseconds(5000)),
                            "a failing action must report one completion");
        test_support::check(action_calls.load() == 1, "a failing action must not be retried");
        test_support::check(seen == ErrorCode::invocation_failed, "the action failure must be preserved");

        action_calls.store(0);
        completions.store(0);
        auto thrown = Il2Cpp::perform(
                [&action_calls] {
                    action_calls.fetch_add(1);
                    throw std::runtime_error("action exploded");
                },
                [&](Result<void> result) {
                    /// 先写结果再发布完成计数，避免调用线程看到完成却读到旧结果。
                    if (!result) seen = result.error().code;
                    completions.fetch_add(1);
                },
                {});
        test_support::check(thrown.has_value(), "a throwing action must still be accepted");
        test_support::check(test_support::wait_until([&] { return completions.load() == 1; },
                                                     std::chrono::milliseconds(5000)),
                            "a throwing action must report one completion");
        test_support::check(action_calls.load() == 1, "a throwing action must not be retried");
        test_support::check(seen == ErrorCode::internal_error, "an unclassified exception must map to internal_error");
        test_support::check(diagnostics.contains("category=failure"), "the default diagnostic must report the failure");
    }
}
