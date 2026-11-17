#include "hooks/managed_provider_internal.hpp"

namespace INB::detail {
namespace {

Result<void*> unavailable_install(void*, void*, void*) {
    return Result<void*>::failure(make_error(ErrorCode::hook_backend_unavailable,
                                             "managed replacement requires the Frida Gum backend"));
}

Result<void> unavailable_revert(void*) {
    return Result<void>::failure(make_error(ErrorCode::hook_backend_unavailable,
                                            "managed replacement requires the Frida Gum backend"));
}

bool unavailable_flush(void*) { return false; }

const ReplacementRecordHeader* unavailable_current_record() { return nullptr; }

const ManagedGumProvider kProvider{false, unavailable_install, unavailable_revert, unavailable_flush,
                                   unavailable_current_record};

} /// namespace

/// OFF 构建用同一组符号保持可链接；查询与调用路径不因此被阻断。
const ManagedGumProvider& managed_gum_provider() { return kProvider; }

} /// namespace INB::detail
