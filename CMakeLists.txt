cmake_minimum_required(VERSION 3.19)

project(il2cpp_native_bridge LANGUAGES CXX)

include(cmake/CompilerWarnings.cmake)

find_package(Threads REQUIRED)

# 作为子目录被消费时默认不构建测试，避免把库内测试注册到宿主工程的 CTest。
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IL2CPP_NATIVE_BRIDGE_TESTS_DEFAULT ON)
else ()
    set(IL2CPP_NATIVE_BRIDGE_TESTS_DEFAULT OFF)
endif ()
option(IL2CPP_NATIVE_BRIDGE_BUILD_TESTS
        "Build host-side unit tests"
        ${IL2CPP_NATIVE_BRIDGE_TESTS_DEFAULT})
option(IL2CPP_NATIVE_BRIDGE_WERROR "Treat library warnings as errors" OFF)
option(IL2CPP_NATIVE_BRIDGE_SANITIZE
        "Sanitizer list for the library and its tests, for example address,undefined"
        "")

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
        src/metadata/handles.cpp
        src/metadata/class_query.cpp
        src/metadata/method_query.cpp
        src/metadata/field_access.cpp
        src/invoke/method_invoke.cpp
        src/hooks/entry_point_resolver.cpp
        src/hooks/hook_registration.cpp
)
add_library(il2cpp_native_bridge::il2cpp_native_bridge ALIAS il2cpp_native_bridge)

target_compile_features(il2cpp_native_bridge PUBLIC cxx_std_20)
il2cpp_native_bridge_apply_cxx_standard(il2cpp_native_bridge)
il2cpp_native_bridge_enable_warnings(il2cpp_native_bridge)
target_include_directories(il2cpp_native_bridge
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(il2cpp_native_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(il2cpp_native_bridge PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

if (IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND)
    include(cmake/FridaGum.cmake)
    il2cpp_native_bridge_add_frida_gum_backend()
endif ()

if (IL2CPP_NATIVE_BRIDGE_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif ()
