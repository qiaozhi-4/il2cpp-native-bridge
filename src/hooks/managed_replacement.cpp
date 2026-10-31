#include "hooks/managed_provider_internal.hpp"

#include "facade/facade_internal.hpp"
#include "hooks/abi_profile.hpp"
#include "hooks/hook_internal.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace il2cpp_native_bridge::detail {
namespace {

/// 同时占用 managed 状态的上限；与契约的 256 个名额一致。
constexpr std::size_t kManagedSlotLimit = 256;

struct ManagedRecord {
    /// 必须位于最前：thunk 与 ReplacementCall 只读这里。
    ReplacementRecordHeader header;
    void* target{};
    std::atomic<int> state{static_cast<int>(ReplacementState::installing)};
    std::atomic_bool restore_requested{};
    /// 回调失败累计次数；用于节流报告。
    std::atomic<int> failures{};
    std::shared_ptr<RuntimeContext> context;
    std::string path;
};

struct ManagedRegistry {
    std::mutex mutex;
    /// 强拥有：不依赖调用方保活。
    std::unordered_map<void*, std::unique_ptr<ManagedRecord>> active;
};

/// 与旧注册表同理：进程生命周期内不析构，避免退出期访问。
ManagedRegistry& registry() {
    static ManagedRegistry* instance = new ManagedRegistry;
    return *instance;
}

void report_callback_failure(void* record) {
    auto* managed = static_cast<ManagedRecord*>(record);
    const int count = managed->failures.fetch_add(1, std::memory_order_relaxed) + 1;
    /// 节流：首次与每第 100 次报告，避免高频异常把 stderr 刷满。
    if (count != 1 && count % 100 != 0) return;
    report_diagnostic(ErrorStage::replacement, managed->path, "callback",
                      "the replacement callback threw an exception (count=" + std::to_string(count) + ")");
}

ManagedRecord* find_record(void* target) {
    auto& managed = registry();
    const auto found = managed.active.find(target);
    return found == managed.active.end() ? nullptr : found->second.get();
}

Result<void*> entry_of(const MethodDescriptor& descriptor, const std::string& path) {
    auto entry = resolve_method_entry(descriptor.native);
    if (!entry) {
        fail(entry.error().code, ErrorStage::replacement, path, entry.error().message);
    }
    return entry;
}

/// 恢复并确认释放；未确认时进入 draining 并抛 restore_pending。
void perform_revert(ManagedRecord* record) {
    const auto& provider = managed_gum_provider();
    record->state.store(static_cast<int>(ReplacementState::reverting));

    auto reverted = provider.revert(record->target);
    if (!reverted) {
        record->state.store(static_cast<int>(ReplacementState::active));
        fail(reverted.error().code, ErrorStage::replacement, record->path, reverted.error().message);
    }

    /// flush 必须在锁外调用：它可能与正在退出的回调同步。
    if (!provider.flush(record->target)) {
        record->state.store(static_cast<int>(ReplacementState::draining));
        fail(ErrorCode::restore_pending, ErrorStage::restore, record->path,
             "the replacement is still draining; retry revert after in-flight calls return");
    }

    {
        auto& managed = registry();
        std::lock_guard lock(managed.mutex);
        managed.active.erase(record->target);
    }
}

} /// namespace

Result<void*> resolve_method_entry(const Il2CppMethodInfo* method) {
    if (method == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::invalid_argument, "MethodInfo is missing"));
    }
    return resolve_entry_point_address(method);
}

bool managed_replacement_active(void* target) {
    if (target == nullptr) return false;
    auto& managed = registry();
    std::lock_guard lock(managed.mutex);
    return managed.active.find(target) != managed.active.end();
}

ReplacementScope::ReplacementScope() {
    const auto* header = current_replacement_record();
    if (header == nullptr) return;
    /// 记录头位于 ManagedRecord 最前，可以安全地还原。
    const auto* record = reinterpret_cast<const ManagedRecord*>(header);
    if (record->context == nullptr) return;
    try {
        scope_ = new AccessScope(record->context);
    } catch (...) {
        scope_ = nullptr;
    }
}

ReplacementScope::~ReplacementScope() { delete static_cast<AccessScope*>(scope_); }

const ReplacementRecordHeader* current_replacement_record() {
    const auto& provider = managed_gum_provider();
    return provider.current_record == nullptr ? nullptr : provider.current_record();
}

void install_replacement(const ReplacementRequest& request) {
    const auto& signature = request.signature;
    const auto& view = require_scope_for(signature.context, signature.generation, signature.path);

    /// 后端不可用时先报 missing_api：此时解析入口或 bootstrap 都没有意义。
    const auto& provider = managed_gum_provider();
    if (!provider.available) {
        fail(ErrorCode::missing_api, ErrorStage::replacement, signature.path,
             "managed replacement requires the Frida Gum backend");
    }

    /// profile 是安装的前置关口：没有证据的 family 不进入 Gum，也不解析入口。
    auto profile = resolve_abi_profile(view);
    if (!profile) {
        fail(profile.error().code, ErrorStage::replacement, signature.path, profile.error().message);
    }
    if (profile.value().support == ProfileSupport::rejected) {
        fail(ErrorCode::unsupported_runtime, ErrorStage::replacement, signature.path,
             "the ABI profile '" + profile.value().family + "' has no verified native shape");
    }
    /// 只有偏移 0 有生成代码证据；其他偏移不猜。
    if (profile.value().entry_offset != 0) {
        fail(ErrorCode::unsupported_runtime, ErrorStage::replacement, signature.path,
             "the ABI profile entry offset is not verified");
    }

    auto entry = entry_of(signature.descriptor, signature.path);
    void* target = entry.value();

    if (legacy_hook_active(target)) {
        fail(ErrorCode::owner_conflict, ErrorStage::replacement, signature.path,
             "the target is owned by a legacy Hook");
    }

    auto record = std::make_unique<ManagedRecord>();
    record->header.callback = request.callback;
    record->header.report_failure = &report_callback_failure;
    record->header.original = nullptr;
    record->header.is_static = signature.descriptor.is_static;
    record->target = target;
    record->context = signature.context;
    record->path = signature.path;

    /// 记录在 replace 之前入表：安装返回前就可能进入回调，并发 revert 也必须能找到它。
    ManagedRecord* stored = nullptr;
    {
        auto& managed = registry();
        std::lock_guard lock(managed.mutex);
        if (managed.active.size() >= kManagedSlotLimit) {
            fail(ErrorCode::resource_exhausted, ErrorStage::replacement, signature.path,
                 "the managed replacement budget is exhausted");
        }
        auto inserted = managed.active.emplace(target, std::move(record));
        if (!inserted.second) {
            fail(ErrorCode::owner_conflict, ErrorStage::replacement, signature.path,
                 "the target already has a managed replacement");
        }
        stored = inserted.first->second.get();
    }

    void* replacement_entry = signature.descriptor.is_static ? request.static_entry : request.instance_entry;
    auto installed = provider.install(target, replacement_entry, stored);
    if (!installed) {
        auto& managed = registry();
        std::lock_guard lock(managed.mutex);
        managed.active.erase(target);
        fail(installed.error().code, ErrorStage::replacement, signature.path, installed.error().message);
    }

    stored->header.original = installed.value();

    /// installing 期间收到的恢复请求：在发布 active 之前补做，避免记录被并发 revert 释放后继续使用。
    if (stored->restore_requested.load(std::memory_order_acquire)) {
        perform_revert(stored);
        return;
    }

    stored->state.store(static_cast<int>(ReplacementState::active));
}

void revert_replacement(const std::shared_ptr<RuntimeContext>& context,
                        std::uint64_t generation,
                        const std::vector<MethodDescriptor>& candidates,
                        const std::string& path) {
    (void) require_scope_for(context, generation, path);
    if (candidates.size() != 1) {
        fail(ErrorCode::ambiguous_match, ErrorStage::replacement, path,
             "the method must be unique before it can be restored");
    }

    auto entry = entry_of(candidates.front(), path);
    void* target = entry.value();

    ManagedRecord* record = nullptr;
    {
        auto& managed = registry();
        std::lock_guard lock(managed.mutex);
        record = find_record(target);
    }
    if (record == nullptr) {
        if (legacy_hook_active(target)) {
            fail(ErrorCode::owner_conflict, ErrorStage::replacement, path,
                 "the target is owned by a legacy Hook");
        }
        return;  // 没有 managed 项：重复恢复成功
    }

    if (static_cast<ReplacementState>(record->state.load()) == ReplacementState::installing) {
        record->restore_requested.store(true, std::memory_order_release);
        fail(ErrorCode::restore_pending, ErrorStage::restore, path,
             "the replacement is still installing; restore was requested");
    }

    perform_revert(record);
}

ReplacementState replacement_state_of(const std::shared_ptr<RuntimeContext>& context,
                                      std::uint64_t generation,
                                      const std::vector<MethodDescriptor>& candidates,
                                      const std::string& path) {
    (void) require_scope_for(context, generation, path);
    if (candidates.size() != 1) {
        fail(ErrorCode::ambiguous_match, ErrorStage::replacement, path,
             "the method must be unique before its state can be read");
    }

    auto entry = resolve_method_entry(candidates.front().native);
    if (!entry) return ReplacementState::not_installed;

    auto& managed = registry();
    std::lock_guard lock(managed.mutex);
    const auto* record = find_record(entry.value());
    return record == nullptr ? ReplacementState::not_installed
                             : static_cast<ReplacementState>(record->state.load());
}

} /// namespace il2cpp_native_bridge::detail

namespace il2cpp_native_bridge {

void Method::revert() const {
    detail::revert_replacement(context_, generation_, candidates_, declaring_class_name_ + "::" + name_);
}

ReplacementState Method::replacement_state() const {
    return detail::replacement_state_of(context_, generation_, candidates_, declaring_class_name_ + "::" + name_);
}

} /// namespace il2cpp_native_bridge
