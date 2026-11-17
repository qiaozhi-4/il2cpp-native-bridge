/// 正向探针：unity_types.hpp 必须自包含，且 UnityVector2 能用于带类型的 Hook 签名。
/// 若布局断言、托管类型名映射或头文件依赖被破坏，这里会在配置阶段编译失败。
#include "il2cpp_native_bridge/unity_types.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <string_view>

using namespace il2cpp_native_bridge;

static_assert(managed_type_name<UnityVector2> == "UnityEngine.Vector2");

void on_move(ReplacementCall<UnityVector2, float, bool*>& call, float delta_time, bool* is_hanging) {
    (void) call;
    (void) delta_time;
    (void) is_hanging;
}

int main() { return 0; }
