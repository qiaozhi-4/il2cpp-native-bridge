#pragma once

#include "il2cpp_native_bridge/il2cpp.hpp"
#include "il2cpp_native_bridge/result.hpp"

struct Il2CppMethodInfo;

namespace INB::detail {

    /**
     * 固定 Gum provider 的最小接缝。
     *
     * 真实 provider 与 OFF 构建的 unavailable 实现提供同一组符号；registry 只依赖这里，
     * 不直接引用 Gum 类型，也不假设 provider 一定可用。
     */
    struct ManagedGumProvider {
        bool available{};
        Result<void*> (*install)(void* target, void* replacement, void* data){};
        Result<void> (*revert)(void* target){};
        /// 确认该地址不再有线程停留在 trampoline 内；true 表示可以释放记录。
        bool (*flush)(void* target){};
        /// 当前线程正在执行的 replacement 记录；不在回调内时为空。
        const ReplacementRecordHeader* (*current_record)(){};
    };

    const ManagedGumProvider& managed_gum_provider();

    /// 解析 MethodInfo 的实现入口；不可执行时返回错误。
    Result<void*> resolve_method_entry(const Il2CppMethodInfo* method);

    /// 该地址是否被 managed 替换占用。
    bool managed_replacement_active(void* target);

    /// 撤销 managed 替换；没有记录时直接返回，重复撤销不算失败。
    /// 目标被旧 Hook 占用时抛 owner_conflict；仍有调用停留在旧入口时抛 restore_pending。
    void revert_replacement(const std::shared_ptr<RuntimeContext>& context,
                            std::uint64_t generation,
                            const std::vector<MethodDescriptor>& candidates,
                            const std::string& path);

    /// 读取当前 managed 状态；没有记录时返回 not_installed。
    ReplacementState replacement_state_of(const std::shared_ptr<RuntimeContext>& context,
                                          std::uint64_t generation,
                                          const std::vector<MethodDescriptor>& candidates,
                                          const std::string& path);

} /// namespace INB::detail
