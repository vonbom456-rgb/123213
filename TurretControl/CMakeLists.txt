cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(TurretControl LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "TurretControl targets the Windows ASE server and must be built with MSVC.")
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

option(TURRETCONTROL_WITH_PERMISSIONS "Build against the optional Permissions plugin" OFF)
set(PERMISSIONS_ROOT "" CACHE PATH "Path to ASE-Plugins/Permissions root when permissions support is enabled")

set(ARKAPI_LIB "${ARKAPI_ROOT}/out_lib/ArkApi.lib")
if(NOT EXISTS "${ARKAPI_LIB}")
    message(FATAL_ERROR "ArkApi.lib not found at ${ARKAPI_LIB}; use the matching AseApi 3.56 tree/package")
endif()

add_library(TurretControl SHARED
    Source/TurretControl.cpp
    Source/MiniJson.h
)

target_compile_features(TurretControl PRIVATE cxx_std_17)
target_compile_definitions(TurretControl PRIVATE NOMINMAX _WINDOWS _USRDLL)
target_include_directories(TurretControl PRIVATE
    "${ARKAPI_ROOT}/version/Core/Public"
    "${ARKAPI_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
)
target_link_libraries(TurretControl PRIVATE "${ARKAPI_LIB}")

if(TURRETCONTROL_WITH_PERMISSIONS)
    if(NOT PERMISSIONS_ROOT AND DEFINED ENV{PERMISSIONS_ROOT})
        set(PERMISSIONS_ROOT "$ENV{PERMISSIONS_ROOT}")
    endif()
    if(NOT PERMISSIONS_ROOT)
        message(FATAL_ERROR "TURRETCONTROL_WITH_PERMISSIONS=ON requires PERMISSIONS_ROOT")
    endif()
    set(PERMISSIONS_LIB "${PERMISSIONS_ROOT}/out_lib/Permissions.lib")
    if(NOT EXISTS "${PERMISSIONS_LIB}")
        message(FATAL_ERROR "Permissions.lib not found at ${PERMISSIONS_LIB}")
    endif()
    target_compile_definitions(TurretControl PRIVATE TURRETCONTROL_WITH_PERMISSIONS=1)
    target_include_directories(TurretControl PRIVATE "${PERMISSIONS_ROOT}/Permissions/Public")
    target_link_libraries(TurretControl PRIVATE "${PERMISSIONS_LIB}" delayimp)
    target_link_options(TurretControl PRIVATE "/DELAYLOAD:Permissions.dll")
    set(TC_PLUGIN_INFO "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/PluginInfo.permissions.json")
else()
    target_compile_definitions(TurretControl PRIVATE TURRETCONTROL_WITH_PERMISSIONS=0)
    set(TC_PLUGIN_INFO "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/PluginInfo.json")
endif()

set(TC_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/TurretControl")
set_target_properties(TurretControl PROPERTIES
    OUTPUT_NAME "TurretControl"
    RUNTIME_OUTPUT_DIRECTORY "${TC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TC_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${TC_DIST_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${TC_DIST_DIR}"
)

if(MSVC)
    target_compile_options(TurretControl PRIVATE /W3 /EHsc)
    set_property(TARGET TurretControl PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_custom_command(TARGET TurretControl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/config.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/TurretControl/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TC_PLUGIN_INFO}"
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/TurretControl/PluginInfo.json"
)
