cmake_minimum_required(VERSION 3.20)
project(FluxCrawl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)

add_library(fluxcrawl_core INTERFACE)
add_library(FluxCrawl::core ALIAS fluxcrawl_core)
target_include_directories(fluxcrawl_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(fluxcrawl_core INTERFACE cxx_std_20)

add_executable(fluxcrawl src/main.cpp)
target_link_libraries(fluxcrawl PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl)

include(CTest)
