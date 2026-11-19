#include "runtime_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace INB {
    namespace {

        RuntimeSnapshot snapshot_without_refresh(const std::shared_ptr<detail::RuntimeContext>& context) {
            if (context == nullptr) return {};
            std::lock_guard lock(context->mutex);
            return context->snapshot;
        }

        Result<void> run_when_ready(const std::shared_ptr<detail::RuntimeContext>& context,
                                    const std::function<Result<void>()>& action,
                                    const RuntimeWaitOptions& options) {
            const auto start_time = std::chrono::steady_clock::now();
            bool timeout_reported = false;

            while (true) {
                auto refreshed = detail::refresh_context(context);
                if (refreshed) {
                    auto attachment = detail::acquire_thread(context);
                    if (attachment) return action();
                    if (attachment.error().code != ErrorCode::runtime_not_ready) {
                        return Result<void>::failure(attachment.error());
                    }
                } else if (refreshed.error().code != ErrorCode::runtime_not_ready) {
                    return Result<void>::failure(refreshed.error());
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time);
                if (!timeout_reported && options.timeout.count() > 0 && elapsed >= options.timeout) {
                    timeout_reported = true;
                    if (options.on_timeout) {
                        try {
                            options.on_timeout(snapshot_without_refresh(context), elapsed);
                        } catch (...) {
                            /// 诊断回调不能取消对 IL2CPP 初始化的等待。
                        }
                    }
                }

                std::this_thread::sleep_for(options.poll_interval);
            }
        }

        Result<void> run_action_safely(const std::shared_ptr<detail::RuntimeContext>& context,
                                       const std::function<Result<void>()>& action,
                                       const RuntimeWaitOptions& options) {
            try {
                return run_when_ready(context, action, options);
            } catch (const std::exception& exception) {
                return Result<void>::failure(make_error(ErrorCode::internal_error,
                    std::string("IL2CPP runtime action threw: ") + exception.what()));
            } catch (...) {
                return Result<void>::failure(make_error(ErrorCode::internal_error,
                    "IL2CPP runtime action threw an unknown exception"));
            }
        }

    } /// namespace

    Result<void> Runtime::perform_when_ready(const std::function<Result<void>()>& action,
                                            std::function<void(Result<void>)> completion,
                                            RuntimeWaitOptions options) const {
        if (!action || !completion) {
            return Result<void>::failure(make_error(ErrorCode::invalid_argument,
                "runtime action and completion callback are required"));
        }
        if (options.poll_interval.count() <= 0 || options.timeout.count() < 0) {
            return Result<void>::failure(make_error(ErrorCode::invalid_argument,
                "runtime polling interval must be positive and timeout must not be negative"));
        }
        if (context_ == nullptr) {
            return Result<void>::failure(make_error(ErrorCode::internal_error, "runtime context is missing"));
        }

        try {
            std::thread([context = context_, action, completion = std::move(completion), options = std::move(options)]() mutable {
                auto result = run_action_safely(context, action, options);
                try {
                    completion(std::move(result));
                } catch (...) {
                    /// 完成回调运行在分离的工作线程，异常不能越出线程入口。
                }
            }).detach();
        } catch (const std::exception& exception) {
            return Result<void>::failure(make_error(ErrorCode::internal_error,
                std::string("failed to start IL2CPP runtime worker: ") + exception.what()));
        }

        return Result<void>::success();
    }

} /// namespace INB
