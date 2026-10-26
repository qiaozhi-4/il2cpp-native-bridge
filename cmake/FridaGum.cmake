set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION "17.23.3" CACHE STRING
        "Pinned official precompiled Frida Gum devkit version")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT "" CACHE PATH
        "Optional override for the bundled official Frida Gum devkit root")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_SHA256
        "1a9d92abe132cee87686df3e95b57d7610d8239b29cd3e5710108fe832727852"
        CACHE STRING
        "Expected SHA-256 of libfrida-gum.a for the pinned version; empty disables the check")

get_filename_component(_il2cpp_native_bridge_bundled_gum_root
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/frida-gum/${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION}/android-arm64"
        ABSOLUTE)

function(il2cpp_native_bridge_add_frida_gum_backend)
    if (ANDROID AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "The Frida Gum devkit integration supports Android arm64-v8a only")
    endif ()

    set(gum_devkit_root "${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT}")
    if (gum_devkit_root STREQUAL "")
        set(gum_devkit_root "${_il2cpp_native_bridge_bundled_gum_root}")
    endif ()

    find_path(gum_include_dir
            NAMES frida-gum.h
            PATHS "${gum_devkit_root}" "${gum_devkit_root}/include"
            PATH_SUFFIXES frida-gum-1.0
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    find_file(gum_archive_path
            NAMES libfrida-gum.a
            PATHS "${gum_devkit_root}" "${gum_devkit_root}/lib" "${gum_devkit_root}/lib/arm64-v8a"
                    "${gum_devkit_root}/lib/aarch64-linux-android"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
    )
    if (NOT gum_include_dir OR NOT gum_archive_path)
        message(FATAL_ERROR
                "Frida Gum devkit is incomplete at '${gum_devkit_root}'; expected frida-gum.h and libfrida-gum.a.\n"
                "The devkit is stored with Git LFS: run 'git lfs pull' in the repository root, "
                "or pass -DIL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT=<extracted devkit>.")
    endif ()

    # 归档文件走 Git LFS：未执行 git lfs pull 时，工作区里是 133 字节的指针文件，
    # 直接链接会在最终链接阶段报错且难以定位，因此在这里用摘要提前拦下。
    if (IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT STREQUAL "" AND IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_SHA256)
        file(SHA256 "${gum_archive_path}" gum_actual_sha256)
        if (NOT gum_actual_sha256 STREQUAL IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_SHA256)
            message(FATAL_ERROR
                    "libfrida-gum.a does not match the pinned digest.\n"
                    "  path:     ${gum_archive_path}\n"
                    "  expected: ${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_SHA256}\n"
                    "  actual:   ${gum_actual_sha256}\n"
                    "Run 'git lfs pull' in the repository root to materialize the archive, "
                    "or pass -DIL2CPP_NATIVE_BRIDGE_FRIDA_GUM_SHA256= to skip this check "
                    "when using a custom devkit.")
        endif ()
    elseif (NOT IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT STREQUAL "")
        message(STATUS "Frida Gum digest check skipped for custom devkit root '${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT}'")
    endif ()

    add_library(il2cpp_native_bridge_frida_gum_upstream STATIC IMPORTED GLOBAL)
    set_target_properties(il2cpp_native_bridge_frida_gum_upstream PROPERTIES
            IMPORTED_LOCATION "${gum_archive_path}"
            INTERFACE_INCLUDE_DIRECTORIES "${gum_include_dir}"
            INTERFACE_COMPILE_DEFINITIONS GUM_STATIC
    )
    if (ANDROID)
        target_link_libraries(il2cpp_native_bridge_frida_gum_upstream INTERFACE log android dl z m)
    endif ()

    add_library(il2cpp_native_bridge_frida_gum STATIC
            src/hooks/frida_gum_hook_backend.cpp
    )
    add_library(il2cpp_native_bridge::frida_gum ALIAS il2cpp_native_bridge_frida_gum)
    target_compile_features(il2cpp_native_bridge_frida_gum PUBLIC cxx_std_20)
    target_include_directories(il2cpp_native_bridge_frida_gum
            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(il2cpp_native_bridge_frida_gum
            PUBLIC il2cpp_native_bridge il2cpp_native_bridge_frida_gum_upstream
    )
    set_target_properties(il2cpp_native_bridge_frida_gum PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
