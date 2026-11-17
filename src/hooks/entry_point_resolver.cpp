#include "hook_internal.hpp"
#include "metadata/metadata_internal.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#include <dlfcn.h>

namespace INB::detail {
namespace {

bool executable_address(void* address) {
    if (address == nullptr) return false;
#if defined(__linux__) || defined(__ANDROID__)
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    while (std::getline(maps, line)) {
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        char permissions[5]{};
        std::istringstream row(line);
        row >> std::hex >> start;
        row.ignore(1, '-');
        row >> std::hex >> end >> permissions;
        if (row && value >= start && value < end && permissions[2] == 'x') return true;
    }
    return false;
#else
    Dl_info info{};
    return dladdr(address, &info) != 0 && info.dli_fbase != nullptr;
#endif
}

} /// namespace

Result<void*> resolve_entry_point_address(const Il2CppMethodInfo* method) {
    if (method == nullptr) {
        return Result<void*>::failure(make_error(ErrorCode::invalid_argument, "MethodInfo is missing"));
    }
    /// IL2CPP 没有公开的方法指针访问器；methodPointer 是 MethodInfo 的第一个字段。
    void* address = nullptr;
    std::memcpy(&address, method, sizeof(address));
    if (!executable_address(address)) {
        return Result<void*>::failure(make_error(ErrorCode::entry_point_unavailable,
            address == nullptr ? "MethodInfo.methodPointer is null"
                               : "MethodInfo.methodPointer is not in an executable mapping"));
    }
    return Result<void*>::success(address);
}

Result<void*> resolve_entry_point(const MethodHandle& method) {
    auto valid = validate_method_handle(method);
    if (!valid) return Result<void*>::failure(valid.error());
    auto attached = acquire_thread(method.declaring_class.context);
    if (!attached) return Result<void*>::failure(attached.error());

    return resolve_entry_point_address(method.native);
}

} /// namespace INB::detail
