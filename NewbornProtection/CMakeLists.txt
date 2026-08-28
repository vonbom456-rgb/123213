cmake_minimum_required(VERSION 3.15)
project(NewbornProtection LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED ARKAPI_ROOT)
  message(FATAL_ERROR "ARKAPI_ROOT is required")
endif()
add_library(NewbornProtection SHARED Source/NewbornProtection.cpp)
target_include_directories(NewbornProtection PRIVATE
  "${ARKAPI_ROOT}/version/Core/Public"
  "${ARKAPI_ROOT}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_directories(NewbornProtection PRIVATE "${ARKAPI_ROOT}/out_lib")
target_link_libraries(NewbornProtection PRIVATE ArkApi)

