/// 正向探针：旧 custom backend 只实现 install/remove 两个纯虚方法。
/// 新能力若给 HookBackend 增加纯虚，这里会编译失败。
#include "il2cpp_native_bridge/hook_backend.hpp"

#include <memory>

using namespace INB;

class LegacyBackend final : public HookBackend {
public:
    Result<void*> install(void* target, void* replacement) override {
        (void) target;
        (void) replacement;
        return Result<void*>::failure(make_error(ErrorCode::hook_backend_unavailable, "unused"));
    }

    Result<void> remove(void* target) override {
        (void) target;
        return Result<void>::success();
    }
};

std::unique_ptr<HookBackend> make_legacy() { return std::make_unique<LegacyBackend>(); }

int main() { return make_legacy() != nullptr ? 0 : 1; }
