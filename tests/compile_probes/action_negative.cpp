/// 负向探针：perform 的 action 只能返回 void 或 Result<void>。
#include "il2cpp_native_bridge/il2cpp.hpp"

void probe_perform() {
    (void) INB::Il2Cpp::perform([] { return 42; });
}

int main() { return 0; }
