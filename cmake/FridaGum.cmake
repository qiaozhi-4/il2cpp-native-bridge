set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_VERSION "17.23.3" CACHE STRING
        "Pinned official precompiled Frida Gum devkit version")
set(IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT "" CACHE PATH
        "Optional override for the bundled official Frida Gum devkit root")

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
                "Frida Gum devkit is incomplete at '${gum_devkit_root}'; expected frida-gum.h and libfrida-gum.a")
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
