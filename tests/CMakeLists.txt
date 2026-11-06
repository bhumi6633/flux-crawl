add_executable(fluxcrawl_version_test version_test.cpp)
target_link_libraries(fluxcrawl_version_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_version_test)
add_test(NAME core.version COMMAND fluxcrawl_version_test)

add_executable(fluxcrawl_url_test url_test.cpp)
target_link_libraries(fluxcrawl_url_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_url_test)
add_test(NAME core.url COMMAND fluxcrawl_url_test)
set_tests_properties(core.url PROPERTIES TIMEOUT 10)

add_executable(fluxcrawl_html_parser_test html_parser_test.cpp)
# libxml2 is linked here only for the test's external-resource loader guard.
target_link_libraries(fluxcrawl_html_parser_test PRIVATE FluxCrawl::core LibXml2::LibXml2)
fluxcrawl_enable_warnings(fluxcrawl_html_parser_test)
add_test(NAME core.html_parser COMMAND fluxcrawl_html_parser_test)
set_tests_properties(core.html_parser PROPERTIES TIMEOUT 10)

add_executable(fluxcrawl_crawl_state_test crawl_state_test.cpp)
target_link_libraries(fluxcrawl_crawl_state_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_crawl_state_test)
add_test(NAME core.crawl_state COMMAND fluxcrawl_crawl_state_test)
set_tests_properties(core.crawl_state PROPERTIES TIMEOUT 10)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(fluxcrawl_http_client_test http_client_test.cpp)
target_link_libraries(fluxcrawl_http_client_test PRIVATE FluxCrawl::core CURL::libcurl)
fluxcrawl_enable_warnings(fluxcrawl_http_client_test)
add_test(NAME core.http_client COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl_http_client_test>")
set_tests_properties(core.http_client PROPERTIES TIMEOUT 30)

add_executable(fluxcrawl_sequential_crawler_test sequential_crawler_test.cpp)
target_link_libraries(fluxcrawl_sequential_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_sequential_crawler_test)
add_test(NAME core.sequential_crawler COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl_sequential_crawler_test>")
set_tests_properties(core.sequential_crawler PROPERTIES TIMEOUT 30)

add_executable(fluxcrawl_thread_pool_crawler_test thread_pool_crawler_test.cpp)
target_link_libraries(fluxcrawl_thread_pool_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_thread_pool_crawler_test)
add_test(NAME core.thread_pool_crawler COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl_thread_pool_crawler_test>")
set_tests_properties(core.thread_pool_crawler PROPERTIES TIMEOUT 90)

add_test(NAME cli.crawl COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl>" --crawl-cli)
set_tests_properties(cli.crawl PROPERTIES TIMEOUT 30)

add_test(NAME cli.threaded COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl>" --threaded-cli)
set_tests_properties(cli.threaded PROPERTIES TIMEOUT 30)

foreach(case IN ITEMS help version invalid help_invalid seed normalized_seed invalid_seed unsupported_seed missing extra_seed bad_depth negative_pages overflow_depth missing_value bad_engine missing_engine zero_workers negative_workers overflow_workers missing_workers irrelevant_workers zero_concurrency negative_concurrency overflow_concurrency missing_concurrency bad_concurrency irrelevant_concurrency threaded_concurrency async_workers async_zero_pages)
    add_test(NAME cli.${case}
        COMMAND "${CMAKE_COMMAND}"
            "-DFLUXCRAWL=$<TARGET_FILE:fluxcrawl>"
            "-DCASE=${case}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
    set_tests_properties(cli.${case} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(fluxcrawl_async_crawler_test async_crawler_test.cpp)
target_link_libraries(fluxcrawl_async_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_async_crawler_test)
add_test(NAME core.async_crawler COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl_async_crawler_test>")
set_tests_properties(core.async_crawler PROPERTIES TIMEOUT 90)

add_test(NAME cli.async COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl>" --async-cli)
set_tests_properties(cli.async PROPERTIES TIMEOUT 30)

add_executable(fluxcrawl_metrics_test metrics_test.cpp)
target_link_libraries(fluxcrawl_metrics_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_metrics_test)
add_test(NAME core.metrics COMMAND fluxcrawl_metrics_test)

add_test(NAME benchmark.server_graph COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmark_server_test.py")

add_executable(fluxcrawl_benchmark_test benchmark_test.cpp)
target_link_libraries(fluxcrawl_benchmark_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_benchmark_test)
add_test(NAME benchmark.arithmetic_csv COMMAND fluxcrawl_benchmark_test)
add_test(NAME benchmark.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmark_integration.py" "$<TARGET_FILE:fluxcrawl_benchmark_test>" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(benchmark.integration PROPERTIES TIMEOUT 90)

add_executable(fluxcrawl_profiling_test profiling_test.cpp)
target_link_libraries(fluxcrawl_profiling_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_profiling_test)
add_test(NAME core.profiling COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_server.py" "$<TARGET_FILE:fluxcrawl_profiling_test>")
set_tests_properties(core.profiling PROPERTIES TIMEOUT 90)

add_executable(fluxcrawl_host_scheduler_test host_scheduler_test.cpp)
target_link_libraries(fluxcrawl_host_scheduler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_host_scheduler_test)
add_test(NAME core.host_scheduler COMMAND fluxcrawl_host_scheduler_test)
set_tests_properties(core.host_scheduler PROPERTIES TIMEOUT 10)

add_executable(fluxcrawl_host_crawler_test host_crawler_test.cpp)
target_link_libraries(fluxcrawl_host_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_host_crawler_test)
add_test(NAME scheduling.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/host_integration.py" "$<TARGET_FILE:fluxcrawl_host_crawler_test>")
set_tests_properties(scheduling.integration PROPERTIES TIMEOUT 120)
add_test(NAME scheduling.stress COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/host_integration.py" "$<TARGET_FILE:fluxcrawl_host_crawler_test>" --stress)
set_tests_properties(scheduling.stress PROPERTIES TIMEOUT 120 LABELS stress)

add_test(NAME cli.scheduling COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scheduling_cli_test.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(cli.scheduling PROPERTIES TIMEOUT 30)

add_executable(fluxcrawl_retry_policy_test retry_policy_test.cpp)
target_link_libraries(fluxcrawl_retry_policy_test PRIVATE FluxCrawl::core CURL::libcurl)
fluxcrawl_enable_warnings(fluxcrawl_retry_policy_test)
add_test(NAME core.retry_policy COMMAND fluxcrawl_retry_policy_test)
set_tests_properties(core.retry_policy PROPERTIES TIMEOUT 10)

add_executable(fluxcrawl_retry_crawler_test retry_crawler_test.cpp)
target_link_libraries(fluxcrawl_retry_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_retry_crawler_test)
add_test(NAME retry.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/retry_integration.py" "$<TARGET_FILE:fluxcrawl_retry_crawler_test>")
set_tests_properties(retry.integration PROPERTIES TIMEOUT 120)
add_test(NAME retry.stress COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/retry_integration.py" "$<TARGET_FILE:fluxcrawl_retry_crawler_test>" --stress)
set_tests_properties(retry.stress PROPERTIES TIMEOUT 120 LABELS stress)

add_test(NAME cli.retry COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/retry_cli_test.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(cli.retry PROPERTIES TIMEOUT 90)

add_executable(fluxcrawl_robots_policy_test robots_policy_test.cpp)
target_link_libraries(fluxcrawl_robots_policy_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_robots_policy_test)
add_test(NAME core.robots_policy COMMAND fluxcrawl_robots_policy_test)
set_tests_properties(core.robots_policy PROPERTIES TIMEOUT 10)

add_executable(fluxcrawl_robots_crawler_test robots_crawler_test.cpp)
target_link_libraries(fluxcrawl_robots_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_robots_crawler_test)
add_test(NAME robots.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/robots_integration.py" "$<TARGET_FILE:fluxcrawl_robots_crawler_test>")
set_tests_properties(robots.integration PROPERTIES TIMEOUT 180)
add_test(NAME robots.stress COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/robots_integration.py" "$<TARGET_FILE:fluxcrawl_robots_crawler_test>" --stress)
set_tests_properties(robots.stress PROPERTIES TIMEOUT 180 LABELS stress)

add_test(NAME cli.robots COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/robots_cli_test.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(cli.robots PROPERTIES TIMEOUT 90)

add_executable(fluxcrawl_checkpoint_test checkpoint_test.cpp)
target_link_libraries(fluxcrawl_checkpoint_test PRIVATE FluxCrawl::core)
target_include_directories(fluxcrawl_checkpoint_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
fluxcrawl_enable_warnings(fluxcrawl_checkpoint_test)
add_test(NAME core.checkpoint COMMAND fluxcrawl_checkpoint_test "${CMAKE_CURRENT_BINARY_DIR}/unit-checkpoint.chk")
set_tests_properties(core.checkpoint PROPERTIES TIMEOUT 15)

add_executable(fluxcrawl_checkpoint_crawler_test checkpoint_crawler_test.cpp)
target_link_libraries(fluxcrawl_checkpoint_crawler_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_checkpoint_crawler_test)
add_test(NAME checkpoint.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/checkpoint_integration.py" "$<TARGET_FILE:fluxcrawl_checkpoint_crawler_test>")
set_tests_properties(checkpoint.integration PROPERTIES TIMEOUT 180)

add_test(NAME cli.checkpoint COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/checkpoint_cli_test.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(cli.checkpoint PROPERTIES TIMEOUT 180)

add_executable(fluxcrawl_sqlite_store_test sqlite_store_test.cpp)
target_link_libraries(fluxcrawl_sqlite_store_test PRIVATE FluxCrawl::core ${FLUXCRAWL_SQLITE_TARGET})
fluxcrawl_enable_warnings(fluxcrawl_sqlite_store_test)
add_test(NAME core.sqlite_store COMMAND fluxcrawl_sqlite_store_test "${CMAKE_CURRENT_BINARY_DIR}/sqlite-unit")
set_tests_properties(core.sqlite_store PROPERTIES TIMEOUT 20)

add_test(NAME sqlite.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/sqlite_integration.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(sqlite.integration PROPERTIES TIMEOUT 240)
add_test(NAME sqlite.stress COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/sqlite_integration.py" "$<TARGET_FILE:fluxcrawl>" --stress)
set_tests_properties(sqlite.stress PROPERTIES TIMEOUT 240 LABELS stress)

add_executable(fluxcrawl_backpressure_test backpressure_test.cpp)
target_link_libraries(fluxcrawl_backpressure_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_backpressure_test)
add_test(NAME core.backpressure COMMAND fluxcrawl_backpressure_test)
set_tests_properties(core.backpressure PROPERTIES TIMEOUT 30)
add_test(NAME backpressure.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/backpressure_integration.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(backpressure.integration PROPERTIES TIMEOUT 180)

option(FLUXCRAWL_SLOW_TESTS "Include 10k-resource backpressure stress in CTest" OFF)
if(FLUXCRAWL_SLOW_TESTS)
    add_test(NAME backpressure.stress COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/backpressure_stress.py" "$<TARGET_FILE:fluxcrawl>")
    set_tests_properties(backpressure.stress PROPERTIES TIMEOUT 300 LABELS stress)
endif()

add_executable(fluxcrawl_dedup_test dedup_test.cpp)
target_link_libraries(fluxcrawl_dedup_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_dedup_test)
add_test(NAME core.dedup COMMAND fluxcrawl_dedup_test "${CMAKE_CURRENT_BINARY_DIR}/dedup-unit")
set_tests_properties(core.dedup PROPERTIES TIMEOUT 30)
add_test(NAME dedup.integration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/dedup_integration.py" "$<TARGET_FILE:fluxcrawl>")
set_tests_properties(dedup.integration PROPERTIES TIMEOUT 240)
if(FLUXCRAWL_SLOW_TESTS)
    add_test(NAME dedup.stress COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/dedup_stress.py" "$<TARGET_FILE:fluxcrawl>")
    set_tests_properties(dedup.stress PROPERTIES TIMEOUT 720 LABELS stress)
endif()
