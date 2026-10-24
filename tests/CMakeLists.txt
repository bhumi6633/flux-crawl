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
