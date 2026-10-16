cmake_minimum_required(VERSION 3.19)

project(il2cpp_native_bridge_standalone LANGUAGES CXX)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." il2cpp-native-bridge)

add_executable(il2cpp_native_bridge_standalone src/main.cpp)
target_compile_features(il2cpp_native_bridge_standalone PRIVATE cxx_std_20)
target_link_libraries(il2cpp_native_bridge_standalone PRIVATE
        il2cpp_native_bridge_frida_gum
)
