#include "facade_internal.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

namespace INB::detail {
namespace {

/// 当前线程最内层的作用域；嵌套只记录前一层，不改变外层租约。
thread_local AccessScope* tls_scope_top = nullptr;

std::mutex& diagnostic_mutex() {
    static std::mutex mutex;
    return mutex;
}

DiagnosticSink& diagnostic_sink() {
    static DiagnosticSink sink;
    return sink;
}

std::mutex& facade_context_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<RuntimeContext>& facade_context_slot() {
    static std::shared_ptr<RuntimeContext> slot;
    return slot;
}

/// 生产环境共用 Runtime 的进程根 context：宿主配好的符号解析器等只需配置一次。
/// 门面（perform）与 Runtime::current() 共享同一 context 与 generation，避免各自发现 libil2cpp。
std::shared_ptr<RuntimeContext> production_facade_context() {
    return current_runtime_context();
}

std::string_view stage_name(ErrorStage stage) {
    switch (stage) {
        case ErrorStage::none: return "none";
        case ErrorStage::submit: return "submit";
        case ErrorStage::readiness: return "readiness";
        case ErrorStage::scope: return "scope";
        case ErrorStage::lookup: return "lookup";
        case ErrorStage::bind: return "bind";
        case ErrorStage::invoke: return "invoke";
        case ErrorStage::replacement: return "replacement";
        case ErrorStage::restore: return "restore";
        case ErrorStage::completion: return "completion";
    }
    return "none";
}

} /// namespace

void set_diagnostic_sink(DiagnosticSink sink) {
    std::lock_guard lock(diagnostic_mutex());
    diagnostic_sink() = std::move(sink);
}

void report_diagnostic(ErrorStage stage,
                       std::string_view path,
                       std::string_view category,
                       std::string_view message) {
    std::string line = "[il2cpp-native-bridge] stage=";
    line += stage_name(stage);
    line += " path=";
    line += path.empty() ? std::string_view("-") : path;
    line += " category=";
    line += category;
    line += " message=";
    line += message;

    DiagnosticSink sink;
    {
        std::lock_guard lock(diagnostic_mutex());
        sink = diagnostic_sink();
    }
    if (sink) {
        try {
            sink(line);
        } catch (...) {
            /// 诊断接收器失败不能改变请求结果。
        }
        return;
    }
    std::fputs(line.c_str(), stderr);
    std::fputc('\n', stderr);
}

std::shared_ptr<RuntimeContext> current_facade_context() {
    std::lock_guard lock(facade_context_mutex());
    auto& slot = facade_context_slot();
    return slot != nullptr ? slot : production_facade_context();
}

void install_facade_context(std::shared_ptr<RuntimeContext> context) {
    std::lock_guard lock(facade_context_mutex());
    facade_context_slot() = std::move(context);
}

void reset_facade_context() {
    std::lock_guard lock(facade_context_mutex());
    facade_context_slot().reset();
}

Result<ReadyView> core_ready_view(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) {
        return Result<ReadyView>::failure(make_error(ErrorCode::internal_error, ErrorStage::readiness, {},
                                                     "runtime context is missing"));
    }

    std::lock_guard lock(context->mutex);
    if (context->core.state != RuntimeState::ready || context->domain == nullptr) {
        const std::string message = context->core.last_error.empty()
                ? std::string("IL2CPP runtime is not ready for the facade")
                : context->core.last_error;
        return Result<ReadyView>::failure(make_error(ErrorCode::runtime_not_ready, ErrorStage::readiness, {}, message));
    }

    ReadyView view;
    view.context = context;
    view.api = context->api;
    view.domain = context->domain;
    view.generation = context->snapshot.generation;
    return Result<ReadyView>::success(std::move(view));
}

AccessScope::AccessScope(std::shared_ptr<RuntimeContext> context) : context_(std::move(context)) {
    if (context_ == nullptr) {
        throw BridgeError(make_error(ErrorCode::internal_error, ErrorStage::scope, {}, "runtime context is missing"));
    }

    if (auto* current = current_access_scope(); current != nullptr) {
        if (current->context_ != context_) {
            throw BridgeError(make_error(ErrorCode::scope_violation, ErrorStage::scope, {},
                                         "a different runtime context is already active on this thread"));
        }
        view_ = current->view_;
        previous_ = current;
        tls_scope_top = this;
        return;
    }

    auto ready = core_ready_view(context_);
    if (!ready) throw BridgeError(ready.error());
    view_ = std::move(ready.value());

    auto lease = acquire_thread(context_, view_.api, view_.domain);
    if (!lease) throw BridgeError(lease.error());
    lease_ = std::move(lease).take_value();
    previous_ = nullptr;
    tls_scope_top = this;
}

AccessScope::~AccessScope() {
    tls_scope_top = previous_;
    /// lease_ 析构时按 owns_attachment 只分离本次取得的线程。
}

AccessScope* current_access_scope() noexcept { return tls_scope_top; }

const ReadyView& require_access_scope() {
    if (tls_scope_top == nullptr) {
        throw BridgeError(make_error(ErrorCode::scope_violation, ErrorStage::scope, {},
                                     "facade runtime operation requires an active perform or replacement scope"));
    }
    return tls_scope_top->view();
}

void fail(ErrorCode code, ErrorStage stage, std::string path, std::string message) {
    throw BridgeError(make_error(code, stage, std::move(path), std::move(message)));
}

const ReadyView& require_scope_for(const std::shared_ptr<RuntimeContext>& context,
                                   std::uint64_t generation,
                                   std::string_view path) {
    const auto& view = require_access_scope();
    if (view.context != context) {
        fail(ErrorCode::scope_violation, ErrorStage::scope, std::string(path),
             "the handle belongs to a different runtime context");
    }
    if (view.generation != generation) {
        fail(ErrorCode::stale_handle, ErrorStage::lookup, std::string(path),
             "the handle belongs to a previous runtime generation");
    }
    return view;
}

FacadeExecutor::FacadeExecutor(std::weak_ptr<RuntimeContext> context) : context_(std::move(context)) {}

FacadeExecutor::~FacadeExecutor() { stop(); }

Result<void> FacadeExecutor::submit(std::function<Result<void>()> action,
                                    Completion completion,
                                    PerformOptions options) {
    auto request = std::make_shared<PerformRequest>();
    request->action = std::move(action);
    request->completion = std::move(completion);
    request->diagnostic_after = options.diagnostic_after;
    request->on_wait = std::move(options.on_wait);
    request->submitted_at = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    if (stopping_) {
        return Result<void>::failure(make_error(ErrorCode::internal_error, ErrorStage::submit, {},
                                                "the facade executor is stopping"));
    }
    if (queue_.size() >= kPerformQueueCapacity) {
        return Result<void>::failure(make_error(ErrorCode::resource_exhausted, ErrorStage::submit, {},
                                                "the perform queue is full"));
    }
    if (!started_) {
        try {
            worker_ = std::thread([this] { run(); });
            started_ = true;
        } catch (const std::exception& error) {
            return Result<void>::failure(make_error(ErrorCode::internal_error, ErrorStage::submit, {},
                                                    std::string("failed to start the facade worker: ") + error.what()));
        }
    }

    request->request_id = next_request_id_++;
    queue_.push_back(std::move(request));
    wake_.notify_one();
    return Result<void>::success();
}

void FacadeExecutor::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (!worker_.joinable()) return;
    /// 协调线程自己触发停机时不能自 join。
    if (std::this_thread::get_id() == worker_.get_id()) {
        worker_.detach();
        return;
    }
    worker_.join();
}

void FacadeExecutor::run() {
    while (true) {
        std::shared_ptr<PerformRequest> request;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            /// 等待就绪期间请求仍占用队列名额，出队即代表开始执行。
            request = queue_.front();
        }

        auto ready = wait_for_core_ready(*request);
        if (!ready) {
            pop_front(request);
            finish(*request, Result<void>::failure(ready.error()));
            continue;
        }

        pop_front(request);
        run_action(*request, ready.value());
    }
}

void FacadeExecutor::pop_front(const std::shared_ptr<PerformRequest>& request) {
    std::lock_guard lock(mutex_);
    if (!queue_.empty() && queue_.front() == request) queue_.pop_front();
}

void FacadeExecutor::run_action(PerformRequest& request, const ReadyView& view) {
    Result<void> outcome = Result<void>::success();
    try {
        AccessScope scope(view.context);
        outcome = request.action();
    } catch (const BridgeError& error) {
        outcome = Result<void>::failure(error.error());
    } catch (const std::exception& error) {
        outcome = Result<void>::failure(make_error(ErrorCode::internal_error, ErrorStage::none, {},
                                                   std::string("perform action threw: ") + error.what()));
    } catch (...) {
        outcome = Result<void>::failure(make_error(ErrorCode::internal_error, ErrorStage::none, {},
                                                   "perform action threw an unknown exception"));
    }
    finish(request, outcome);
}

void FacadeExecutor::finish(PerformRequest& request, const Result<void>& outcome) {
    /// 结果确定后立即释放 action/options 捕获；completion 本身不在作用域内执行。
    request.action = nullptr;
    request.on_wait = nullptr;
    if (!outcome) {
        report_diagnostic(outcome.error().stage, outcome.error().path, "failure", outcome.error().message);
    }

    if (request.completion) {
        auto completion = std::move(request.completion);
        request.completion = nullptr;
        try {
            completion(outcome);
        } catch (...) {
            /// 完成回调失败只报告，不重跑 action，也不产生第二次 completion。
            report_diagnostic(ErrorStage::completion, {}, "callback", "the completion callback threw");
        }
    }
}

Result<ReadyView> FacadeExecutor::wait_for_core_ready(PerformRequest& request) {
    while (true) {
        const auto context = context_.lock();
        if (context == nullptr) {
            return Result<ReadyView>::failure(make_error(ErrorCode::internal_error, ErrorStage::readiness, {},
                                                         "the runtime context was destroyed before the request ran"));
        }

        auto refreshed = refresh_core(context);
        if (refreshed) {
            auto view = core_ready_view(context);
            if (view) return view;
        } else if (refreshed.error().code != ErrorCode::runtime_not_ready) {
            /// 核心能力不可继续：请求失败一次，不执行 action。
            return Result<ReadyView>::failure(refreshed.error());
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - request.submitted_at);
        if (!request.wait_reported && request.diagnostic_after.count() > 0 &&
            elapsed >= request.diagnostic_after) {
            request.wait_reported = true;
            if (request.on_wait) {
                const auto snapshot = diagnostic_snapshot(*context);
                try {
                    request.on_wait(snapshot, elapsed);
                } catch (...) {
                    report_diagnostic(ErrorStage::readiness, {}, "callback", "the wait diagnostic callback threw");
                }
            }
        }

        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, kPerformPollInterval, [this] { return stopping_; });
        if (stopping_) {
            return Result<ReadyView>::failure(make_error(ErrorCode::internal_error, ErrorStage::readiness, {},
                                                         "the facade executor stopped while waiting for IL2CPP"));
        }
    }
}

RuntimeSnapshot FacadeExecutor::diagnostic_snapshot(RuntimeContext& context) {
    std::lock_guard lock(context.mutex);
    RuntimeSnapshot snapshot;
    snapshot.state = context.core.state;
    snapshot.generation = context.snapshot.generation;
    snapshot.capabilities = context.core.capabilities;
    snapshot.missing_apis = context.core.missing_apis;
    snapshot.last_error = context.core.last_error;
    return snapshot;
}

void stop_facade_executor(const std::shared_ptr<RuntimeContext>& context) {
    if (context == nullptr) return;
    FacadeExecutor* executor = nullptr;
    {
        std::lock_guard lock(context->mutex);
        executor = context->facade_executor.get();
    }
    if (executor != nullptr) executor->stop();
}

Result<void> submit_perform_request(std::function<Result<void>()> action,
                                    Completion completion,
                                    PerformOptions options) {
    if (!action) {
        const auto error = make_error(ErrorCode::invalid_argument, ErrorStage::submit, {},
                                      "perform action must not be empty");
        report_diagnostic(error.stage, error.path, "rejected", error.message);
        return Result<void>::failure(error);
    }
    if (options.diagnostic_after.count() < 0) {
        const auto error = make_error(ErrorCode::invalid_argument, ErrorStage::submit, {},
                                      "the perform diagnostic threshold must not be negative");
        report_diagnostic(error.stage, error.path, "rejected", error.message);
        return Result<void>::failure(error);
    }

    const auto context = current_facade_context();
    if (context == nullptr) {
        const auto error = make_error(ErrorCode::internal_error, ErrorStage::submit, {},
                                      "the facade runtime context is missing");
        report_diagnostic(error.stage, error.path, "rejected", error.message);
        return Result<void>::failure(error);
    }

    FacadeExecutor* executor = nullptr;
    {
        std::lock_guard lock(context->mutex);
        if (context->facade_executor == nullptr) {
            context->facade_executor = std::make_unique<FacadeExecutor>(context);
        }
        executor = context->facade_executor.get();
    }

    auto submitted = executor->submit(std::move(action), std::move(completion), std::move(options));
    if (!submitted) {
        report_diagnostic(submitted.error().stage, submitted.error().path, "rejected", submitted.error().message);
    }
    return submitted;
}

} /// namespace INB::detail
