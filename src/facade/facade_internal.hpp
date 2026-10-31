#pragma once

#include "il2cpp_native_bridge/il2cpp.hpp"
#include "runtime/il2cpp_api.hpp"
#include "runtime/runtime_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace il2cpp_native_bridge::detail {

/// 一次 perform 请求的排队上限，不含正在执行的请求。
inline constexpr std::size_t kPerformQueueCapacity = 128;
/// 等待 runtime 就绪时的统一检查间隔；不对每个请求暴露。
inline constexpr std::chrono::milliseconds kPerformPollInterval{100};

/**
 * 已校验的就绪视图。
 *
 * 在作用域入口建立一次，之后内部各层直接使用，不再重复判定就绪或判空。
 */
struct ReadyView {
    std::shared_ptr<RuntimeContext> context;
    Api api;
    Il2CppDomain* domain{};
    std::uint64_t generation{};
};

/// 建立 core 就绪视图；未就绪时返回对应错误，不执行任何 runtime 调用。
Result<ReadyView> core_ready_view(const std::shared_ptr<RuntimeContext>& context);

/**
 * 线程局部访问作用域。
 *
 * 同 context 的嵌套作用域复用外层租约；不同 context 直接拒绝。析构只分离本次附加的线程。
 */
class AccessScope {
public:
    explicit AccessScope(std::shared_ptr<RuntimeContext> context);
    ~AccessScope();

    AccessScope(const AccessScope&) = delete;
    AccessScope& operator=(const AccessScope&) = delete;
    AccessScope(AccessScope&&) = delete;

    [[nodiscard]] const ReadyView& view() const noexcept { return view_; }

private:
    std::shared_ptr<RuntimeContext> context_;
    ReadyView view_;
    ThreadLease lease_;
    AccessScope* previous_{};
};

/// 当前线程的作用域视图；不在作用域内时抛 scope_violation。
const ReadyView& require_access_scope();
/// 当前线程最内层作用域，没有时返回空。
AccessScope* current_access_scope() noexcept;

/// 统一的失败出口：门面在第一个失败环节抛出结构化错误。
[[noreturn]] void fail(ErrorCode code, ErrorStage stage, std::string path, std::string message);

/// 校验句柄属于当前作用域与同一 runtime 代际，返回本次可用视图。
const ReadyView& require_scope_for(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   std::string_view path);

/// 一次 perform 请求；由队列持有直到完成。
struct PerformRequest {
    std::uint64_t request_id{};
    std::function<Result<void>()> action;
    Completion completion;
    std::chrono::milliseconds diagnostic_after{10000};
    std::function<void(const RuntimeSnapshot&, std::chrono::milliseconds)> on_wait;
    std::chrono::steady_clock::time_point submitted_at{};
    bool wait_reported{false};
};

/**
 * 每个 context 一个的协调执行器。
 *
 * 只借用 context（weak_ptr），不形成 context → executor → context 的强引用环；
 * 队列锁只保护接收/领取与停机标志，runtime 调用与用户回调都在锁外。
 */
class FacadeExecutor {
public:
    explicit FacadeExecutor(std::weak_ptr<RuntimeContext> context);
    ~FacadeExecutor();

    FacadeExecutor(const FacadeExecutor&) = delete;
    FacadeExecutor& operator=(const FacadeExecutor&) = delete;

    /// 接收一次请求；返回失败时不留下队列项、action 或 completion。
    Result<void> submit(std::function<Result<void>()> action, Completion completion, PerformOptions options);

    /// 停机并等待协调线程退出；不从协调线程自身 join。
    void stop();

private:
    void run();
    void run_action(PerformRequest& request, const ReadyView& view);
    void finish(PerformRequest& request, const Result<void>& outcome);
    void pop_front(const std::shared_ptr<PerformRequest>& request);
    Result<ReadyView> wait_for_core_ready(PerformRequest& request);
    RuntimeSnapshot diagnostic_snapshot(RuntimeContext& context);

    std::weak_ptr<RuntimeContext> context_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<PerformRequest>> queue_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
    std::uint64_t next_request_id_{1};
};

/// 诊断输出接收器；测试用它捕获默认诊断，生产不设置时写 stderr。
using DiagnosticSink = std::function<void(std::string_view)>;

void set_diagnostic_sink(DiagnosticSink sink);
void report_diagnostic(ErrorStage stage,
                       std::string_view path,
                       std::string_view category,
                       std::string_view message);

/// 生产 current context 的进程根；测试可用 install/reset 替换。
std::shared_ptr<RuntimeContext> current_facade_context();
void install_facade_context(std::shared_ptr<RuntimeContext> context);
void reset_facade_context();

/// 测试 Harness 使用：停机并 join 该 context 的执行器；没有执行器时为空操作。
void stop_facade_executor(const std::shared_ptr<RuntimeContext>& context);

} /// namespace il2cpp_native_bridge::detail
