#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/runtime_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace il2cpp_native_bridge;

namespace {

/// 控制 runtime 何时报告已加载程序集，用来构造「先超时、后就绪」的时序。
std::atomic<bool> g_assemblies_available{false};
detail::Api g_base_api;
Il2CppThread g_thread;

const Il2CppAssembly** gated_assemblies(const Il2CppDomain* domain, std::size_t* count) {
    if (!g_assemblies_available.load(std::memory_order_acquire)) {
        if (count != nullptr) *count = 0;
        return nullptr;
    }
    return g_base_api.domain_get_assemblies(domain, count);
}

Il2CppThread* stub_thread_current() { return &g_thread; }
Il2CppThread* stub_thread_attach(Il2CppDomain*) { return &g_thread; }
void stub_thread_detach(Il2CppThread*) {}

/// 组装一个带线程桩、程序集可开关的测试 runtime。
/// 传入空的 domain，使初始状态为 initializing，就绪与否完全由 gated_assemblies 控制。
Runtime make_gated_runtime(fake_il2cpp::Fixture& fixture) {
    g_base_api = fixture.api();
    auto api = fixture.api();
    api.domain_get_assemblies = &gated_assemblies;
    api.thread_current = &stub_thread_current;
    api.thread_attach = &stub_thread_attach;
    api.thread_detach = &stub_thread_detach;
    return detail::make_testing_runtime(api, nullptr);
}

void wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} /// namespace

void run_perform_when_ready_tests() {
    /// 超时回调只报一次，就绪后 action 与 completion 各执行一次。
    {
        fake_il2cpp::Fixture fixture;
        g_assemblies_available.store(false, std::memory_order_release);
        const auto runtime = make_gated_runtime(fixture);

        std::atomic<int> action_calls{0};
        std::atomic<int> timeout_calls{0};
        std::mutex mutex;
        std::condition_variable completed_signal;
        bool completed = false;
        bool completion_ok = false;

        RuntimeWaitOptions options;
        options.poll_interval = std::chrono::milliseconds(5);
        options.timeout = std::chrono::milliseconds(15);
        options.on_timeout = [&timeout_calls](const RuntimeSnapshot&, std::chrono::milliseconds) {
            timeout_calls.fetch_add(1, std::memory_order_relaxed);
        };

        auto started = runtime.perform_when_ready(
                [&action_calls] {
                    action_calls.fetch_add(1, std::memory_order_relaxed);
                    return Result<void>::success();
                },
                [&](Result<void> result) {
                    {
                        std::lock_guard lock(mutex);
                        completed = true;
                        completion_ok = result.has_value();
                    }
                    completed_signal.notify_all();
                },
                options);
        test_support::check(started.has_value(), "starting the background wait must succeed");

        wait_until([&] { return timeout_calls.load(std::memory_order_relaxed) > 0; },
                   std::chrono::milliseconds(2000));
        test_support::check(timeout_calls.load(std::memory_order_relaxed) == 1,
                            "the timeout callback must fire exactly once");

        g_assemblies_available.store(true, std::memory_order_release);
        {
            std::unique_lock lock(mutex);
            test_support::check(completed_signal.wait_for(lock, std::chrono::milliseconds(5000),
                                                          [&] { return completed; }),
                                "completion must run after the runtime becomes ready");
        }

        test_support::check(action_calls.load(std::memory_order_relaxed) == 1,
                            "the action must run exactly once");
        test_support::check(completion_ok, "the completion must receive success");
        test_support::check(timeout_calls.load(std::memory_order_relaxed) == 1,
                            "the timeout callback must not fire again after the runtime is ready");
    }

    /// action 抛出的异常必须转成错误结果交给 completion，而不是逃出工作线程。
    {
        fake_il2cpp::Fixture fixture;
        g_assemblies_available.store(true, std::memory_order_release);
        const auto runtime = make_gated_runtime(fixture);

        std::mutex mutex;
        std::condition_variable completed_signal;
        bool completed = false;
        ErrorCode completion_code = ErrorCode::invalid_argument;

        RuntimeWaitOptions options;
        options.poll_interval = std::chrono::milliseconds(5);
        options.timeout = std::chrono::milliseconds(0);

        auto started = runtime.perform_when_ready(
                []() -> Result<void> { throw std::runtime_error("action exploded"); },
                [&](Result<void> result) {
                    {
                        std::lock_guard lock(mutex);
                        completed = true;
                        completion_code = result.has_value() ? ErrorCode::invalid_argument : result.error().code;
                    }
                    completed_signal.notify_all();
                },
                options);
        test_support::check(started.has_value(), "starting a throwing action must succeed");

        std::unique_lock lock(mutex);
        test_support::check(completed_signal.wait_for(lock, std::chrono::milliseconds(5000),
                                                      [&] { return completed; }),
                            "completion must run after the action throws");
        test_support::check(completion_code == ErrorCode::internal_error,
                            "a throwing action must surface internal_error");
    }

    /// 参数校验：空回调与非法轮询参数直接拒绝，不启动后台线程。
    {
        fake_il2cpp::Fixture fixture;
        g_assemblies_available.store(true, std::memory_order_release);
        const auto runtime = make_gated_runtime(fixture);

        auto no_action = runtime.perform_when_ready({}, [](Result<void>) {}, {});
        test_support::check(!no_action && no_action.error().code == ErrorCode::invalid_argument,
                            "an empty action must be rejected");

        auto no_completion = runtime.perform_when_ready([] { return Result<void>::success(); }, {}, {});
        test_support::check(!no_completion && no_completion.error().code == ErrorCode::invalid_argument,
                            "an empty completion must be rejected");

        RuntimeWaitOptions zero_poll;
        zero_poll.poll_interval = std::chrono::milliseconds(0);
        auto bad_poll = runtime.perform_when_ready(
                [] { return Result<void>::success(); }, [](Result<void>) {}, zero_poll);
        test_support::check(!bad_poll && bad_poll.error().code == ErrorCode::invalid_argument,
                            "a non-positive poll interval must be rejected");

        RuntimeWaitOptions negative_timeout;
        negative_timeout.timeout = std::chrono::milliseconds(-1);
        auto bad_timeout = runtime.perform_when_ready(
                [] { return Result<void>::success(); }, [](Result<void>) {}, negative_timeout);
        test_support::check(!bad_timeout && bad_timeout.error().code == ErrorCode::invalid_argument,
                            "a negative timeout must be rejected");
    }
}
