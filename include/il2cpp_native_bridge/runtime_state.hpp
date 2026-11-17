#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace INB {

/// IL2CPP runtime 的就绪阶段；ready 之前的阶段都不应发起托管调用。
enum class RuntimeState {
    /// 尚未取得 libil2cpp 模块，或注入的符号解析器尚未就绪。
    waiting_for_module,
    /// 模块和导出已就绪，正在获取 domain 并确认已加载程序集。
    initializing,
    /// 完成初始化，可以执行托管操作。
    ready,
    /// 缺少必需导出，当前运行时不受支持。
    unsupported,
};

/**
 * 某一时刻 runtime 状态的只读快照，供诊断与超时回调使用。
 *
 * generation 标识当前 runtime 代际：缓存句柄在代际变化后失效；ready 之前字段可能为空。
 */
struct RuntimeSnapshot {
    /// 当前就绪阶段。
    RuntimeState state{RuntimeState::waiting_for_module};
    /// runtime 代际，从 0 开始，就绪后自增。
    std::uint64_t generation{0};
    /// 当前已确认可用的能力名称。
    std::vector<std::string> capabilities;
    /// 缺失的必需导出名称。
    std::vector<std::string> missing_apis;
    /// 缺失的可选导出名称。
    std::vector<std::string> missing_optional_apis;
    /// 最近一次就绪失败的诊断信息，成功时为空。
    std::string last_error;
};

} /// namespace INB
