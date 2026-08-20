cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(DamageAlerts LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "DamageAlerts targets the Windows ASE server and must be built with MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build x64 only.")
endif()

set(ARKAPI_ROOT "" CACHE PATH "Path to ArkServerApi/AseApi 3.56 SDK/source root")
if(NOT ARKAPI_ROOT)
    if(DEFINED ENV{ARKAPI_ROOT})
        set(ARKAPI_ROOT "$ENV{ARKAPI_ROOT}")
    else()
        message(FATAL_ERROR "Set -DARKAPI_ROOT=C:/path/to/AseApi or ARKAPI_ROOT environment variable")
    endif()
endif()

set(ARKAPI_LIB "${ARKAPI_ROOT}/out_lib/ArkApi.lib")
if(NOT EXISTS "${ARKAPI_LIB}")
    message(FATAL_ERROR "ArkApi.lib not found at ${ARKAPI_LIB}; use the matching AseApi 3.56 tree/package")
endif()

add_library(DamageAlerts SHARED
    Source/DamageAlerts.cpp
    Source/MiniJson.h
)

target_compile_features(DamageAlerts PRIVATE cxx_std_17)
target_compile_definitions(DamageAlerts PRIVATE NOMINMAX _WINDOWS _USRDLL)
target_include_directories(DamageAlerts PRIVATE
    "${ARKAPI_ROOT}/version/Core/Public"
    "${ARKAPI_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
)
target_link_libraries(DamageAlerts PRIVATE "${ARKAPI_LIB}")

set(PC_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/DamageNumbers")
set_target_properties(DamageAlerts PROPERTIES
    OUTPUT_NAME "DamageNumbers"
    RUNTIME_OUTPUT_DIRECTORY "${PC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${PC_DIST_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${PC_DIST_DIR}"
)

if(MSVC)
    target_compile_options(DamageAlerts PRIVATE /W3 /EHsc)
    set_property(TARGET DamageAlerts PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_custom_command(TARGET DamageAlerts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/config.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/DamageNumbers/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/PluginInfo.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/DamageNumbers/PluginInfo.json"
)
