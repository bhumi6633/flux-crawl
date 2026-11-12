cmake_minimum_required(VERSION 3.20)
project(FluxCrawl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Instrumentation.cmake)
include(cmake/CompilerWarnings.cmake)

find_package(CURL 7.85 REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 3.24 REQUIRED)
if(TARGET SQLite3::SQLite3)
    set(FLUXCRAWL_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(FLUXCRAWL_SQLITE_TARGET SQLite::SQLite3)
endif()

add_library(fluxcrawl_core
    src/url.cpp
    src/bloom_filter.cpp
    src/seen_set.cpp
    src/html_parser.cpp
    src/crawl_frontier.cpp
    src/persistence_runtime.cpp
    src/checkpoint_runtime.cpp
    src/sqlite_store.cpp
    src/checkpoint_file.cpp
    src/checkpoint.cpp
    src/crawl_policy.cpp
    src/robots_policy.cpp
    src/retry_policy.cpp
    src/host_scheduler.cpp
    src/crawl_state.cpp
    src/discovery_spool.cpp
    src/curl_transfer.cpp
    src/http_client.cpp
    src/http_response.cpp
    src/crawl_processing.cpp
    src/sequential_crawler.cpp
    src/thread_pool_crawler.cpp
    src/async_crawler.cpp
    src/observability.cpp
    src/metrics.cpp
    src/benchmark.cpp
    src/benchmark_server_client.cpp
    src/benchmark_output.cpp)
add_library(FluxCrawl::core ALIAS fluxcrawl_core)
target_include_directories(fluxcrawl_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(fluxcrawl_core PUBLIC cxx_std_20)
target_link_libraries(fluxcrawl_core PRIVATE CURL::libcurl LibXml2::LibXml2 Threads::Threads ${FLUXCRAWL_SQLITE_TARGET})
target_compile_definitions(fluxcrawl_core PRIVATE FLUXCRAWL_BUILD_TYPE="$<CONFIG>")
fluxcrawl_enable_warnings(fluxcrawl_core)

option(FLUXCRAWL_OBSERVABILITY "Build loopback HTTP/WebSocket observability server" ON)
if(FLUXCRAWL_OBSERVABILITY)
    find_package(Boost 1.81 CONFIG REQUIRED COMPONENTS json)
    add_library(fluxcrawl_observability src/observability_server.cpp src/observability_json.cpp)
    target_link_libraries(fluxcrawl_observability PUBLIC FluxCrawl::core PRIVATE Boost::json Threads::Threads)
    target_compile_definitions(fluxcrawl_observability PRIVATE FLUXCRAWL_DASHBOARD_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/dashboard/dist")
    fluxcrawl_enable_warnings(fluxcrawl_observability)
endif()

add_executable(fluxcrawl src/main.cpp src/benchmark_cli.cpp src/checkpoint_cli.cpp)
target_link_libraries(fluxcrawl PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl)
if(FLUXCRAWL_OBSERVABILITY)
    set_property(TARGET fluxcrawl PROPERTY LINK_LIBRARIES fluxcrawl_observability)
    target_compile_definitions(fluxcrawl PRIVATE FLUXCRAWL_OBSERVABILITY=1)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Explicit operational storage measurements; excluded from normal engine benchmarks/tests.
add_executable(fluxcrawl_persistence_scale tools/persistence_scale.cpp)
target_link_libraries(fluxcrawl_persistence_scale PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_persistence_scale)

add_executable(fluxcrawl_dedup_baseline tools/dedup_baseline.cpp)
target_link_libraries(fluxcrawl_dedup_baseline PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_dedup_baseline)
add_executable(fluxcrawl_dedup_scale tools/dedup_scale.cpp)
target_link_libraries(fluxcrawl_dedup_scale PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_dedup_scale)

if(FLUXCRAWL_ENABLE_FUZZING)
    add_subdirectory(fuzz)
endif()

add_executable(fluxcrawl_certification_environment tools/certification_environment.cpp)
target_link_libraries(fluxcrawl_certification_environment PRIVATE FluxCrawl::core CURL::libcurl LibXml2::LibXml2)
fluxcrawl_enable_warnings(fluxcrawl_certification_environment)
