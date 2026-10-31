// 负向探针：typed() 必须拒绝没有托管类型名映射的参数类型。
#include "il2cpp_native_bridge/il2cpp.hpp"

struct UnsupportedProbe {
    int value;
};

void probe(const il2cpp_native_bridge::Method& method) {
    (void) method.typed<void, UnsupportedProbe>();
}

int main() { return 0; }
