#include <exception>
#include <iostream>

void run_runtime_lifecycle_tests();
void run_frida_gum_backend_tests();
void run_member_lookup_tests();
void run_class_cache_tests();
void run_field_access_tests();
void run_invocation_tests();
void run_hook_lifecycle_tests();

int main() {
    try {
        run_runtime_lifecycle_tests();
        run_frida_gum_backend_tests();
        run_member_lookup_tests();
        run_class_cache_tests();
        run_field_access_tests();
        run_invocation_tests();
        run_hook_lifecycle_tests();
        std::cout << "il2cpp-native-bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "il2cpp-native-bridge tests failed: " << error.what() << '\n';
        return 1;
    }
}
