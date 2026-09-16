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

foreach(case IN ITEMS help version invalid help_invalid seed normalized_seed invalid_seed unsupported_seed missing extra_seed)
    add_test(NAME cli.${case}
        COMMAND "${CMAKE_COMMAND}"
            "-DFLUXCRAWL=$<TARGET_FILE:fluxcrawl>"
            "-DCASE=${case}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
    set_tests_properties(cli.${case} PROPERTIES TIMEOUT 10)
endforeach()
