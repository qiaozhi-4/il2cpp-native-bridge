cmake_minimum_required(VERSION 3.19)

project(il2cpp_native_bridge VERSION 0.1.0 LANGUAGES CXX)

include(cmake/CompilerWarnings.cmake)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

# 作为子目录被消费时默认不构建测试、不注册安装规则，避免影响宿主工程。
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IL2CPP_NATIVE_BRIDGE_TOP_LEVEL ON)
else ()
    set(IL2CPP_NATIVE_BRIDGE_TOP_LEVEL OFF)
endif ()

option(IL2CPP_NATIVE_BRIDGE_BUILD_TESTS
        "Build host-side unit tests"
        ${IL2CPP_NATIVE_BRIDGE_TOP_LEVEL})
option(IL2CPP_NATIVE_BRIDGE_INSTALL
        "Generate install rules and package config files"
        ${IL2CPP_NATIVE_BRIDGE_TOP_LEVEL})
option(IL2CPP_NATIVE_BRIDGE_WERROR "Treat library warnings as errors" OFF)
option(IL2CPP_NATIVE_BRIDGE_SANITIZE
        "Sanitizer list for the library and its tests, for example address,undefined"
        "")
option(IL2CPP_NATIVE_BRIDGE_ENABLE_LOGCAT
        "Emit INB-tagged logcat diagnostics for bridge failures in Android Debug builds"
        ${ANDROID})

if (ANDROID)
    set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_DEFAULT ON)
else ()
    set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_DEFAULT OFF)
endif ()
option(IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND
        "Build the Frida Gum Interceptor backend"
        ${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_DEFAULT})

if (ANDROID AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "il2cpp-native-bridge supports Android arm64-v8a only")
endif ()

# 目录级选项只向下生效，不会影响把本库当子目录引入的宿主工程。
# -fno-sanitize-recover=all 让 UBSan 报告直接中止进程，否则只会打印而测试仍算通过。
if (IL2CPP_NATIVE_BRIDGE_SANITIZE)
    add_compile_options(-fsanitize=${IL2CPP_NATIVE_BRIDGE_SANITIZE}
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${IL2CPP_NATIVE_BRIDGE_SANITIZE})
endif ()

add_library(il2cpp_native_bridge STATIC
        src/platform/android/runtime_module.cpp
        src/runtime/export_resolver.cpp
        src/runtime/perform.cpp
        src/runtime/runtime_context.cpp
        src/runtime/thread_attachment.cpp
        src/runtime/thread_attachment_public.cpp
        src/facade/perform.cpp
        src/facade/queries.cpp
        src/facade/invocation.cpp
        src/metadata/handles.cpp
        src/metadata/class_query.cpp
        src/metadata/method_query.cpp
        src/metadata/field_access.cpp
        src/invoke/method_invoke.cpp
        src/hooks/entry_point_resolver.cpp
        src/hooks/abi_profile.cpp
        src/hooks/managed_replacement.cpp
        src/hooks/hook_registration.cpp
)
add_library(il2cpp_native_bridge::il2cpp_native_bridge ALIAS il2cpp_native_bridge)

# 门面消费目标：只转发核心库，保持「门面 → 核心」单向依赖。
# 门面实现随核心静态库编译，消费者不需要额外链接项；Gum 头不经过该目标暴露。
add_library(il2cpp_native_bridge_perform INTERFACE)
add_library(il2cpp_native_bridge::perform ALIAS il2cpp_native_bridge_perform)
target_link_libraries(il2cpp_native_bridge_perform INTERFACE il2cpp_native_bridge)

target_compile_features(il2cpp_native_bridge PUBLIC cxx_std_20)
il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge)
il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge)
target_include_directories(il2cpp_native_bridge
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(il2cpp_native_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(il2cpp_native_bridge PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

# 失败日志：参考 BNM 的编译期开关，只在 Android Debug 构建打 INB 标签的 logcat。
# host 构建与单测不定义 INB_ENABLE_LOGCAT，日志头展开为空操作，行为保持不变。
if (ANDROID AND IL2CPP_NATIVE_BRIDGE_ENABLE_LOGCAT)
    target_compile_definitions(il2cpp_native_bridge PRIVATE
            "$<$<CONFIG:Debug>:INB_ENABLE_LOGCAT>")
    target_link_libraries(il2cpp_native_bridge PRIVATE log)
endif ()

# managed replacement 的 provider 二选一：真实 Gum 或 OFF 构建的 unavailable 实现。
# 两者提供同一组符号，测试目标可以自带真实 provider（配合 Gum 替身）覆盖它。
if (IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND)
    include(cmake/FridaGum.cmake)
    il2cpp_native_bridge_add_frida_gum_backend()
    target_sources(il2cpp_native_bridge PRIVATE src/hooks/managed_gum_provider.cpp)
    target_link_libraries(il2cpp_native_bridge PUBLIC il2cpp_native_bridge_frida_gum_upstream)
else ()
    target_sources(il2cpp_native_bridge PRIVATE src/hooks/unavailable_gum_provider.cpp)
endif ()

# T030 原生验收：只在 Android + 真实 Gum 后端时编译。主机与 OFF 构建不产出该目标，
# 也不注册 CTest —— Gum 运行测试必须在设备上执行，不能混进主机测试。
if (ANDROID AND IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND AND IL2CPP_NATIVE_BRIDGE_BUILD_TESTS)
    add_executable(il2cpp_native_bridge_gum_acceptance tests/runtime/gum_managed_lifecycle_acceptance.cpp)
    target_compile_features(il2cpp_native_bridge_gum_acceptance PRIVATE cxx_std_20)
    il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge_gum_acceptance)
    il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge_gum_acceptance)
    target_link_libraries(il2cpp_native_bridge_gum_acceptance PRIVATE il2cpp_native_bridge_frida_gum_upstream)
endif ()

if (IL2CPP_NATIVE_BRIDGE_INSTALL)
    install(TARGETS il2cpp_native_bridge
            EXPORT il2cpp_native_bridgeTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT il2cpp_native_bridgeTargets
            NAMESPACE il2cpp_native_bridge::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/il2cpp_native_bridge)

    if (TARGET il2cpp_native_bridge_frida_gum)
        set(IL2CPP_NATIVE_BRIDGE_INSTALL_FRIDA_GUM ON)
    else ()
        set(IL2CPP_NATIVE_BRIDGE_INSTALL_FRIDA_GUM OFF)
    endif ()

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
            cmake/il2cpp_native_bridgeConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/il2cpp_native_bridgeConfig.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/il2cpp_native_bridge)
    write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/il2cpp_native_bridgeConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMinorVersion)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/il2cpp_native_bridgeConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/il2cpp_native_bridgeConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/il2cpp_native_bridge)
endif ()

if (IL2CPP_NATIVE_BRIDGE_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif ()
