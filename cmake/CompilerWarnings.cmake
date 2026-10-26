# 对库自身源码启用编译告警。
#
# 第三方头文件（frida-gum.h）通过 IMPORTED 目标的 INTERFACE_INCLUDE_DIRECTORIES 引入，
# 按 CMake 约定按 SYSTEM 处理，不参与本项目告警评估。
function(il2cpp_native_bridge_enable_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if (IL2CPP_NATIVE_BRIDGE_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif ()
        return ()
    endif ()

    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if (IL2CPP_NATIVE_BRIDGE_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif ()
endfunction()

# 固定 C++20 且禁用编译器扩展，避免不同工具链下的语义漂移。
function(il2cpp_native_bridge_apply_cxx_standard target)
    set_target_properties(${target} PROPERTIES
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
endfunction()
