#pragma once

#include "facade/facade_internal.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {

inline void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

/// 捕获默认门面诊断，避免测试输出被 stderr 淹没；析构时恢复默认输出。
class DiagnosticCapture {
public:
    DiagnosticCapture() {
        INB::detail::set_diagnostic_sink([this](std::string_view line) {
            std::lock_guard lock(mutex_);
            lines_.emplace_back(line);
        });
    }

    ~DiagnosticCapture() { INB::detail::set_diagnostic_sink({}); }

    DiagnosticCapture(const DiagnosticCapture&) = delete;
    DiagnosticCapture& operator=(const DiagnosticCapture&) = delete;

    [[nodiscard]] std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }

    [[nodiscard]] bool contains(std::string_view needle) const {
        std::lock_guard lock(mutex_);
        return std::any_of(lines_.begin(), lines_.end(), [needle](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

/**
 * 门面测试运行时：独占一个 context 并安装为当前门面 context。
 *
 * 析构顺序固定为「停机 → join → 释放 context」，避免协调线程仍访问已销毁的数据。
 */
class FacadeHarness {
public:
    explicit FacadeHarness(INB::detail::Api api, Il2CppDomain* domain = nullptr) {
        context_ = INB::detail::make_testing_context(std::move(api), domain);
        context_->injected_api = true;
        INB::detail::install_facade_context(context_);
    }

    ~FacadeHarness() {
        INB::detail::stop_facade_executor(context_);
        INB::detail::reset_facade_context();
    }

    FacadeHarness(const FacadeHarness&) = delete;
    FacadeHarness& operator=(const FacadeHarness&) = delete;

    [[nodiscard]] const std::shared_ptr<INB::detail::RuntimeContext>& context() const noexcept {
        return context_;
    }

private:
    std::shared_ptr<INB::detail::RuntimeContext> context_;
};

/// 轮询等待谓词成立；用于替代靠 sleep 猜时序。
template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// 在 context 锁内读取 core 就绪状态；协调线程会并发改写它。
inline INB::RuntimeState core_state(
        const std::shared_ptr<INB::detail::RuntimeContext>& context) {
    std::lock_guard lock(context->mutex);
    return context->core.state;
}

/// 一次作用域内操作的结果；异常在作用域内被捕获，带回调用线程断言。
struct ScopeOutcome {
    bool ok{false};
    INB::ErrorCode code{INB::ErrorCode::internal_error};
    INB::ErrorStage stage{INB::ErrorStage::none};
    std::string path;
    std::string message;
    std::string name;
    int int_value{};
    float float_value{};
    double double_value{};
    bool bool_value{};
    Il2CppObject* object{};
};

/// 在 perform 作用域内执行操作，等 completion 之后再读取结果。
inline ScopeOutcome run_in_scope(const std::function<void(ScopeOutcome&)>& body) {
    using namespace INB;

    ScopeOutcome outcome;
    std::mutex mutex;
    std::condition_variable signal;
    bool done = false;

    auto submitted = Il2Cpp::perform(
            [&] {
                try {
                    body(outcome);
                    outcome.ok = true;
                } catch (const BridgeError& error) {
                    outcome.code = error.error().code;
                    outcome.stage = error.error().stage;
                    outcome.path = error.error().path;
                    outcome.message = error.error().message;
                }
            },
            [&](Result<void>) {
                std::lock_guard lock(mutex);
                done = true;
                signal.notify_all();
            },
            {});
    if (!submitted) {
        outcome.message = "submitting the scoped request failed";
        return outcome;
    }

    std::unique_lock lock(mutex);
    if (!signal.wait_for(lock, std::chrono::seconds(5), [&] { return done; })) {
        outcome.message = "the scoped request did not complete";
    }
    return outcome;
}

} /// namespace test_support
