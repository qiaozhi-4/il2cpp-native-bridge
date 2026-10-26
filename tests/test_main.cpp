#include <cstring>
#include <exception>
#include <iostream>
#include <string>

void run_runtime_lifecycle_tests();
void run_frida_gum_backend_tests();
void run_member_lookup_tests();
void run_class_cache_tests();
void run_field_access_tests();
void run_invocation_tests();
void run_hook_lifecycle_tests();
void run_hook_slot_tests();
void run_concurrency_tests();
void run_perform_when_ready_tests();

namespace {

struct Group {
    const char* name;
    void (*run)();
};

/// 每个分组单独注册成一条 CTest，便于定位失败范围；`--only` 只跑其中一个。
constexpr Group kGroups[] = {
        {"runtime_lifecycle", run_runtime_lifecycle_tests},
        {"frida_gum_backend", run_frida_gum_backend_tests},
        {"member_lookup", run_member_lookup_tests},
        {"class_cache", run_class_cache_tests},
        {"field_access", run_field_access_tests},
        {"invocation", run_invocation_tests},
        {"hook_lifecycle", run_hook_lifecycle_tests},
        {"hook_slot", run_hook_slot_tests},
        {"concurrency", run_concurrency_tests},
        {"perform_when_ready", run_perform_when_ready_tests},
};

} /// namespace

int main(int argc, char** argv) {
    std::string only;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--list") == 0) {
            for (const auto& group : kGroups) std::cout << group.name << '\n';
            return 0;
        }
        if (std::strcmp(argv[index], "--only") == 0 && index + 1 < argc) {
            only = argv[++index];
        }
    }

    try {
        bool ran_any = false;
        for (const auto& group : kGroups) {
            if (!only.empty() && only != group.name) continue;
            group.run();
            ran_any = true;
        }
        if (!ran_any) {
            std::cerr << "il2cpp-native-bridge tests: unknown group '" << only << "'\n";
            return 2;
        }
        std::cout << "il2cpp-native-bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "il2cpp-native-bridge tests failed: " << error.what() << '\n';
        return 1;
    }
}
