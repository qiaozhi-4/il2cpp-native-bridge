cmake_minimum_required(VERSION 3.19)

project(il2cpp_native_bridge_acceptance LANGUAGES CXX)

if (NOT DEFINED IL2CPP_NATIVE_BRIDGE_SOURCE_DIR)
    message(FATAL_ERROR "Set IL2CPP_NATIVE_BRIDGE_SOURCE_DIR to the library root")
endif ()

set(IL2CPP_NATIVE_BRIDGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if (ANDROID)
    set(IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND ON CACHE BOOL "" FORCE)
else ()
    set(IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND OFF CACHE BOOL "" FORCE)
endif ()

add_subdirectory("${IL2CPP_NATIVE_BRIDGE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bridge")

add_library(il2cpp_native_bridge_acceptance SHARED "${CMAKE_CURRENT_LIST_DIR}/../../../../runtime/set_time_scale_acceptance.cpp")
target_compile_features(il2cpp_native_bridge_acceptance PRIVATE cxx_std_20)
target_link_libraries(il2cpp_native_bridge_acceptance PRIVATE
        il2cpp_native_bridge
        ${CMAKE_DL_LIBS}
)

if (TARGET il2cpp_native_bridge_frida_gum)
    target_compile_definitions(il2cpp_native_bridge_acceptance PRIVATE IL2CPP_NATIVE_BRIDGE_HAS_FRIDA_GUM=1)
    target_link_libraries(il2cpp_native_bridge_acceptance PRIVATE il2cpp_native_bridge_frida_gum)
else ()
    target_compile_definitions(il2cpp_native_bridge_acceptance PRIVATE IL2CPP_NATIVE_BRIDGE_HAS_FRIDA_GUM=0)
endif ()

set_target_properties(il2cpp_native_bridge_acceptance PROPERTIES
        OUTPUT_NAME il2cpp_native_bridge_acceptance
        POSITION_INDEPENDENT_CODE ON
)
