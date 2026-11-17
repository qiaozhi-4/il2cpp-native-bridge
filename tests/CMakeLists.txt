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
        unit/concurrency_test.cpp
        unit/perform_when_ready_test.cpp
        unit/facade_perform_test.cpp
        unit/facade_scope_test.cpp
        unit/facade_query_test.cpp
        unit/facade_invoke_test.cpp
        unit/managed_replacement_test.cpp
        unit/native_abi_profile_test.cpp
        unit/facade_capability_test.cpp
        unit/facade_diagnostics_test.cpp
)
add_library(il2cpp_native_bridge_frida_gum_test_stub STATIC
        frida_gum_stub/fake_frida_gum.cpp
)
target_compile_features(il2cpp_native_bridge_frida_gum_test_stub PUBLIC cxx_std_20)
target_include_directories(il2cpp_native_bridge_frida_gum_test_stub PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/frida_gum_stub"
)

# 测试自带真实 managed provider（配合 Gum 替身）；静态库不会被重复拉入。
add_library(il2cpp_native_bridge_managed_provider_test STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/hooks/managed_gum_provider.cpp"
)
target_compile_features(il2cpp_native_bridge_managed_provider_test PUBLIC cxx_std_20)
target_include_directories(il2cpp_native_bridge_managed_provider_test
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
                "${CMAKE_CURRENT_SOURCE_DIR}/frida_gum_stub"
)
target_link_libraries(il2cpp_native_bridge_managed_provider_test
        PUBLIC il2cpp_native_bridge
        PRIVATE il2cpp_native_bridge_frida_gum_test_stub
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
        il2cpp_native_bridge_managed_provider_test
        il2cpp_native_bridge_frida_gum_test_backend
        il2cpp_native_bridge_frida_gum_test_stub
)
add_test(NAME il2cpp_native_bridge_tests COMMAND il2cpp_native_bridge_tests)

# 每个分组单独注册成一条 CTest，失败时能直接定位到具体范围。
set(IL2CPP_NATIVE_BRIDGE_TEST_GROUPS
        runtime_lifecycle
        frida_gum_backend
        member_lookup
        class_cache
        field_access
        invocation
        hook_lifecycle
        hook_slot
        concurrency
        perform_when_ready
        facade_perform
        facade_query
        facade_invoke
        managed_replacement
        abi_profile
        facade_capability
        facade_diagnostics)
foreach (group IN LISTS IL2CPP_NATIVE_BRIDGE_TEST_GROUPS)
    add_test(NAME "bridge.${group}" COMMAND il2cpp_native_bridge_tests --only "${group}")
endforeach ()

# OFF 消费者：不链接真实 provider，验证查询/调用可用、替换按 missing_api 失败而非未定义符号。
add_executable(il2cpp_native_bridge_off_tests
        unit/facade_off_provider_test.cpp
        runtime/fake_il2cpp.cpp
)
target_compile_features(il2cpp_native_bridge_off_tests PRIVATE cxx_std_20)
il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge_off_tests)
il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge_off_tests)
target_include_directories(il2cpp_native_bridge_off_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(il2cpp_native_bridge_off_tests PRIVATE il2cpp_native_bridge)
add_test(NAME bridge.off_provider COMMAND il2cpp_native_bridge_off_tests)

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

# 编译探针：在独立工程里编译 compile_probes/ 下的源码，用于契约检查（正向与负向共用）。
# 源码放在文件里而不是内联字符串：CMake 列表会按分号切分内联源码，分号在 C++ 里又不可避免。
function(il2cpp_native_bridge_compile_probe name source_file)
    set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/probe-${name}")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.19)\n"
            "project(${name} LANGUAGES CXX)\n"
            "add_executable(${name} \"${CMAKE_CURRENT_SOURCE_DIR}/compile_probes/${source_file}\")\n"
            "target_compile_features(${name} PRIVATE cxx_std_20)\n"
            "target_include_directories(${name} PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/../include\")\n")
    try_compile(${name}_compiles "${probe_dir}/build" "${probe_dir}" ${name}
            OUTPUT_VARIABLE ${name}_output)
    set(${name}_compiles "${${name}_compiles}" PARENT_SCOPE)
    set(${name}_output "${${name}_output}" PARENT_SCOPE)
endfunction()

# 负向：typed() 必须拒绝没有托管类型名映射的参数类型。
il2cpp_native_bridge_compile_probe(probe_typed_negative typed_negative.cpp)
if (probe_typed_negative_compiles)
    message(FATAL_ERROR "typed() must reject unsupported argument types at compile time")
endif ()

# 负向：perform 的 action 只能返回 void 或 Result<void>。
il2cpp_native_bridge_compile_probe(probe_action_negative action_negative.cpp)
if (probe_action_negative_compiles)
    message(FATAL_ERROR "perform() must reject actions that return anything but void or Result<void>")
endif ()

# 正向：旧 custom backend 只实现 install/remove；新增纯虚会让这里编译失败。
il2cpp_native_bridge_compile_probe(probe_legacy_backend legacy_backend.cpp)
if (NOT probe_legacy_backend_compiles)
    message(FATAL_ERROR "the legacy HookBackend contract changed; custom backends must keep compiling: ${probe_legacy_backend_output}")
endif ()

# 正向：unity_types.hpp 必须自包含，且 UnityVector2 的类型名映射与 Hook 签名可用。
il2cpp_native_bridge_compile_probe(probe_unity_types unity_types.cpp)
if (NOT probe_unity_types_compiles)
    message(FATAL_ERROR "unity_types.hpp must stay self-contained with a valid Vector2 mapping: ${probe_unity_types_output}")
endif ()

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME il2cpp_native_bridge_matrix_runner_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/unit/matrix_runner_test.py")
endif ()
