add_executable(il2cpp_native_bridge_tests
        test_main.cpp
        runtime/fake_il2cpp.cpp
        unit/frida_gum_backend_test.cpp
        unit/runtime_lifecycle_test.cpp
        unit/member_lookup_test.cpp
        unit/class_cache_test.cpp
        unit/field_access_test.cpp
        unit/invocation_test.cpp
        unit/hook_lifecycle_test.cpp
        unit/hook_slot_test.cpp
)
add_library(il2cpp_native_bridge_frida_gum_test_stub STATIC
        frida_gum_stub/fake_frida_gum.cpp
)
target_include_directories(il2cpp_native_bridge_frida_gum_test_stub PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/frida_gum_stub"
)

add_library(il2cpp_native_bridge_frida_gum_test_backend STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/hooks/frida_gum_hook_backend.cpp"
)
target_compile_features(il2cpp_native_bridge_frida_gum_test_backend PUBLIC cxx_std_20)
target_include_directories(il2cpp_native_bridge_frida_gum_test_backend
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/frida_gum_stub"
)
target_link_libraries(il2cpp_native_bridge_frida_gum_test_backend
        PUBLIC il2cpp_native_bridge
        PRIVATE il2cpp_native_bridge_frida_gum_test_stub
)

target_compile_features(il2cpp_native_bridge_tests PRIVATE cxx_std_20)
il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge_tests)
il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge_tests)
target_include_directories(il2cpp_native_bridge_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(il2cpp_native_bridge_tests PRIVATE
        il2cpp_native_bridge_frida_gum_test_backend
        il2cpp_native_bridge_frida_gum_test_stub
)
add_test(NAME il2cpp_native_bridge_tests COMMAND il2cpp_native_bridge_tests)

# 静态析构期行为只能在独立进程中观察，因此单独成可执行文件。
add_executable(il2cpp_native_bridge_hook_static_lifetime
        unit/hook_static_lifetime_test.cpp
)
target_compile_features(il2cpp_native_bridge_hook_static_lifetime PRIVATE cxx_std_20)
il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge_hook_static_lifetime)
il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge_hook_static_lifetime)
target_include_directories(il2cpp_native_bridge_hook_static_lifetime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(il2cpp_native_bridge_hook_static_lifetime PRIVATE il2cpp_native_bridge)
add_test(NAME il2cpp_native_bridge_hook_static_lifetime COMMAND il2cpp_native_bridge_hook_static_lifetime)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME il2cpp_native_bridge_matrix_runner_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/unit/matrix_runner_test.py")
endif ()
