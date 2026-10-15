cmake_minimum_required(VERSION 3.24)
project(hdr_parsec_fix LANGUAGES C CXX)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build with MSVC for Windows x64")
endif()
enable_language(ASM_MASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS generate.py pe_exports.py)
execute_process(COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/generate.py" "${GENERATED}"
    COMMAND_ERROR_IS_FATAL ANY)
set(SHADERS)
foreach(stage vs ps)
    set(header "${GENERATED}/cap_hdr_${stage}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${FXC}" /O3 /Ges /nologo /Fh "${header}" /T ${stage}_4_0 /E ${stage}_main
            /Vn cap_hdr_${stage} "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/cap_hdr.hlsl"
        DEPENDS app/shaders/cap_hdr.hlsl VERBATIM)
    list(APPEND SHADERS "${header}")
endforeach()
add_library(parsec_hdr_shim SHARED
    proxy.cpp api_hooks.cpp duplication.cpp capture_source.cpp com_slots.c
    app/cap_hdr.cpp app/cap_wgc.cpp
    "${GENERATED}/forwarders.asm" "${GENERATED}/bcrypt.def" ${SHADERS})
target_include_directories(parsec_hdr_shim PRIVATE "${GENERATED}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(parsec_hdr_shim PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(parsec_hdr_shim PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/Zc:__cplusplus>")
target_link_libraries(parsec_hdr_shim PRIVATE d3d11 dxgi dxguid runtimeobject ole32 oleaut32 user32 advapi32 wtsapi32 userenv)
target_link_options(parsec_hdr_shim PRIVATE /INCREMENTAL:NO)
set_target_properties(parsec_hdr_shim PROPERTIES OUTPUT_NAME bcrypt
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
