#include "runtime_internal.hpp"
#include "il2cpp_native_bridge/bridge.hpp"

namespace il2cpp_native_bridge {

ThreadAttachment::ThreadAttachment(std::unique_ptr<detail::ThreadLease> lease) : lease_(std::move(lease)) {}
ThreadAttachment::ThreadAttachment(ThreadAttachment&&) noexcept = default;
ThreadAttachment& ThreadAttachment::operator=(ThreadAttachment&&) noexcept = default;
ThreadAttachment::~ThreadAttachment() = default;

Result<ThreadAttachment> ThreadAttachment::attach(const Runtime& runtime) {
    auto lease = detail::acquire_thread(runtime.context_);
    if (!lease) return Result<ThreadAttachment>::failure(lease.error());
    return Result<ThreadAttachment>::success(
        ThreadAttachment(std::make_unique<detail::ThreadLease>(lease.take_value())));
}

} /// namespace il2cpp_native_bridge
