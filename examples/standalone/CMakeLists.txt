cmake_minimum_required(VERSION 3.19)

project(il2cpp_native_bridge_standalone LANGUAGES CXX)

# 两种消费方式：
#   默认        —— 把库当子目录构建（源码树内开发用）；
#   USE_PACKAGE —— find_package 消费已安装的包（验证安装/导出是否正确）。
option(IL2CPP_NATIVE_BRIDGE_EXAMPLE_USE_PACKAGE
        "Consume an installed il2cpp_native_bridge package instead of the source tree" OFF)

if (IL2CPP_NATIVE_BRIDGE_EXAMPLE_USE_PACKAGE)
    find_package(il2cpp_native_bridge REQUIRED)
    set(_bridge_target il2cpp_native_bridge::il2cpp_native_bridge)
    set(_gum_target il2cpp_native_bridge::frida_gum)
else ()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." il2cpp-native-bridge)
    set(_bridge_target il2cpp_native_bridge)
    set(_gum_target il2cpp_native_bridge_frida_gum)
endif ()

add_executable(il2cpp_native_bridge_standalone src/main.cpp)
target_compile_features(il2cpp_native_bridge_standalone PRIVATE cxx_std_20)
target_link_libraries(il2cpp_native_bridge_standalone PRIVATE ${_bridge_target})

# Gum 后端只在 ON 构建存在；OFF 消费者只链基础库，不需要 devkit 头文件。
if (TARGET ${_gum_target})
    target_link_libraries(il2cpp_native_bridge_standalone PRIVATE ${_gum_target})
endif ()
