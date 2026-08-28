cmake_minimum_required(VERSION 3.15)
project(InventoryCapacity LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED ARKAPI_ROOT)
  message(FATAL_ERROR "ARKAPI_ROOT is required")
endif()
add_library(InventoryCapacity SHARED Source/InventoryCapacity.cpp)
target_include_directories(InventoryCapacity PRIVATE
  "${ARKAPI_ROOT}/version/Core/Public"
  "${ARKAPI_ROOT}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_directories(InventoryCapacity PRIVATE "${ARKAPI_ROOT}/out_lib")
target_link_libraries(InventoryCapacity PRIVATE ArkApi)
