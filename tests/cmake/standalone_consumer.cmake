if (NOT DEFINED FRIDA_GUM_DEVKIT_ROOT OR FRIDA_GUM_DEVKIT_ROOT STREQUAL "")
    message(FATAL_ERROR "Pass -DFRIDA_GUM_DEVKIT_ROOT=<extracted Frida Gum devkit>")
endif ()
if (NOT DEFINED CONSUMER_BUILD_ROOT OR CONSUMER_BUILD_ROOT STREQUAL "")
    message(FATAL_ERROR "Pass -DCONSUMER_BUILD_ROOT=<temporary build directory>")
endif ()

get_filename_component(repository_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(consumer_build_dir "${CONSUMER_BUILD_ROOT}/standalone-consumer")

execute_process(
        COMMAND "${CMAKE_COMMAND}"
        -S "${repository_root}/examples/standalone"
        -B "${consumer_build_dir}"
        -DIL2CPP_NATIVE_BRIDGE_BUILD_TESTS=OFF
        -DIL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND=ON
        -DIL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT=${FRIDA_GUM_DEVKIT_ROOT}
        RESULT_VARIABLE configure_result
)
if (NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Standalone consumer configure failed with status ${configure_result}")
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${consumer_build_dir}" --target il2cpp_native_bridge_standalone
        RESULT_VARIABLE build_result
)
if (NOT build_result EQUAL 0)
    message(FATAL_ERROR "Standalone consumer build failed with status ${build_result}")
endif ()
