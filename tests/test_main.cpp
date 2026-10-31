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
void run_facade_perform_tests();
void run_facade_scope_tests();
void run_facade_query_tests();
void run_facade_invoke_tests();
void run_managed_replacement_tests();
void run_native_abi_profile_tests();
void run_facade_capability_tests();
void run_facade_diagnostics_tests();

namespace {

/// 门面执行入口与作用域用例同属一个验收分组。
void run_facade_perform_group() {
    run_facade_perform_tests();
    run_facade_scope_tests();
}

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
        {"facade_perform", run_facade_perform_group},
        {"facade_query", run_facade_query_tests},
        {"facade_invoke", run_facade_invoke_tests},
        {"managed_replacement", run_managed_replacement_tests},
        {"abi_profile", run_native_abi_profile_tests},
        {"facade_capability", run_facade_capability_tests},
        {"facade_diagnostics", run_facade_diagnostics_tests},
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
