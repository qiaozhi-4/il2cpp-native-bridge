#pragma once

#include "bridge.hpp"

#include <functional>
#include <memory>

namespace INB {

/**
 * 初始化 Frida Gum（进程内只初始化一次）并创建 Hook backend。
 *
 * @return 可用的 backend；无法取得 Gum interceptor 时返回空指针。
 */
std::shared_ptr<HookBackend> make_frida_gum_hook_backend();

/**
 * 在可用的 Frida Gum backend 作用域内同步执行回调。
 *
 * 回调收到的 shared_ptr 引用只在回调期间借用；需要延长 backend 生命周期时应复制 shared_ptr。
 *
 * @param action 要执行的操作，不能为空。
 * @return action 的结果；action 为空时返回 invalid_argument，backend 不可用时返回
 *         hook_backend_unavailable，action 返回的其他错误原样传回。
 * @throws action 抛出的异常会传给调用方。
 */
Result<void> with_frida_gum_hook_backend(
    const std::function<Result<void>(const std::shared_ptr<HookBackend>&)>& action);

/**
 * 等待 IL2CPP runtime 就绪、附加当前工作线程并创建 Frida Gum backend 后执行 action。
 *
 * 等待策略和超时回调沿用 Runtime::perform_when_ready；runtime 就绪后 action 最多执行一次。
 * backend 不可用或 action 返回错误时，错误交给 completion，不会重试 action。
 * action、completion 和超时回调在后台线程执行，其捕获对象必须在任务结束前保持有效。
 *
 * @return 后台任务启动结果；启动成功只表示任务已排队，最终结果由 completion 接收。
 */
Result<void> perform_when_ready_with_frida_gum_hook_backend(
    const Runtime& runtime,
    std::function<Result<void>(const std::shared_ptr<HookBackend>&)> action,
    std::function<void(Result<void>)> completion,
    RuntimeWaitOptions options = {});

} /// namespace INB
