cmake_minimum_required(VERSION 3.20)
project(FluxCrawl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)

find_package(CURL 7.85 REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(Threads REQUIRED)

add_library(fluxcrawl_core
    src/url.cpp
    src/html_parser.cpp
    src/crawl_frontier.cpp
    src/checkpoint_file.cpp
    src/checkpoint.cpp
    src/crawl_policy.cpp
    src/robots_policy.cpp
    src/retry_policy.cpp
    src/host_scheduler.cpp
    src/crawl_state.cpp
    src/curl_transfer.cpp
    src/http_client.cpp
    src/http_response.cpp
    src/crawl_processing.cpp
    src/sequential_crawler.cpp
    src/thread_pool_crawler.cpp
    src/async_crawler.cpp
    src/metrics.cpp
    src/benchmark.cpp
    src/benchmark_server_client.cpp
    src/benchmark_output.cpp)
add_library(FluxCrawl::core ALIAS fluxcrawl_core)
target_include_directories(fluxcrawl_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(fluxcrawl_core PUBLIC cxx_std_20)
target_link_libraries(fluxcrawl_core PRIVATE CURL::libcurl LibXml2::LibXml2 Threads::Threads)
target_compile_definitions(fluxcrawl_core PRIVATE FLUXCRAWL_BUILD_TYPE="$<CONFIG>")
fluxcrawl_enable_warnings(fluxcrawl_core)

add_executable(fluxcrawl src/main.cpp src/benchmark_cli.cpp)
target_link_libraries(fluxcrawl PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
