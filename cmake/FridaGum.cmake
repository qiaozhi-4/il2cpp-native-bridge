set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION "17.23.3" CACHE STRING
        "Pinned official precompiled Frida Gum devkit version")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT "" CACHE PATH
        "Optional override pointing at an already extracted Frida Gum devkit root")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256
        "81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85"
        CACHE STRING
        "Expected SHA-256 of the bundled devkit archive; empty disables the check")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256
        "1a9d92abe132cee87686df3e95b57d7610d8239b29cd3e5710108fe832727852"
        CACHE STRING
        "Expected SHA-256 of the extracted libfrida-gum.a; empty disables the check")

# 在文件作用域记录仓库根目录：函数体内的 CMAKE_CURRENT_LIST_DIR 指向调用方，
# 不能用来定位仓库内的 third_party。
get_filename_component(_il2cpp_native_bridge_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(il2cpp_native_bridge_add_frida_gum_backend)
    if (ANDROID AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "The Frida Gum devkit integration supports Android arm64-v8a only")
    endif ()

    if (NOT IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT STREQUAL "")
        # 调用方自备已解压的 devkit：沿用文件查找，跳过摘要校验。
        set(gum_devkit_root "${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT}")
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
                    "Frida Gum devkit is incomplete at '${gum_devkit_root}'; "
                    "expected frida-gum.h and libfrida-gum.a")
        endif ()
        message(STATUS "Frida Gum digest checks skipped for custom devkit root '${gum_devkit_root}'")
    else ()
        # 仓库只提交官方 devkit 压缩包（约 10 MB），配置阶段解压到构建目录。
        # 直接提交解压后的 libfrida-gum.a 会让 git 历史多出 77 MB 的二进制。
        set(gum_archive
                "${_il2cpp_native_bridge_root}/third_party/frida-gum/${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION}/android-arm64/frida-gum-devkit-${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION}-android-arm64.tar.xz")
        set(gum_devkit_root
                "${CMAKE_BINARY_DIR}/third_party/frida-gum/${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION}/android-arm64")

        if (NOT EXISTS "${gum_archive}")
            message(FATAL_ERROR
                    "Missing bundled Frida Gum devkit archive at '${gum_archive}'.\n"
                    "Restore third_party/frida-gum/${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION}, or pass "
                    "-DIL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT=<extracted devkit>.")
        endif ()

        if (IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256)
            file(SHA256 "${gum_archive}" gum_archive_sha256)
            if (NOT gum_archive_sha256 STREQUAL IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256)
                message(FATAL_ERROR
                        "Bundled Frida Gum archive does not match the pinned digest.\n"
                        "  path:     ${gum_archive}\n"
                        "  expected: ${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256}\n"
                        "  actual:   ${gum_archive_sha256}\n"
                        "Restore the official 17.23.3 devkit archive, or pass "
                        "-DIL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256= to skip this check.")
            endif ()
        endif ()

        set(gum_extract_stamp "${gum_devkit_root}/.extracted")
        if (NOT EXISTS "${gum_extract_stamp}")
            message(STATUS "Extracting Frida Gum devkit into ${gum_devkit_root}")
            file(REMOVE_RECURSE "${gum_devkit_root}")
            file(ARCHIVE_EXTRACT INPUT "${gum_archive}" DESTINATION "${gum_devkit_root}")
            file(WRITE "${gum_extract_stamp}" "")
        endif ()

        set(gum_include_dir "${gum_devkit_root}")
        set(gum_archive_path "${gum_devkit_root}/libfrida-gum.a")
        if (NOT EXISTS "${gum_archive_path}" OR NOT EXISTS "${gum_devkit_root}/frida-gum.h")
            message(FATAL_ERROR
                    "Frida Gum devkit extraction at '${gum_devkit_root}' is incomplete; "
                    "delete that directory and re-run CMake.")
        endif ()

        if (IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256)
            file(SHA256 "${gum_archive_path}" gum_library_sha256)
            if (NOT gum_library_sha256 STREQUAL IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256)
                message(FATAL_ERROR
                        "Extracted libfrida-gum.a does not match the pinned digest.\n"
                        "  path:     ${gum_archive_path}\n"
                        "  expected: ${IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256}\n"
                        "  actual:   ${gum_library_sha256}\n"
                        "Delete '${gum_devkit_root}' and re-run CMake.")
            endif ()
        endif ()
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
