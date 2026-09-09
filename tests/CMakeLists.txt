add_executable(fluxcrawl_version_test version_test.cpp)
target_link_libraries(fluxcrawl_version_test PRIVATE FluxCrawl::core)
fluxcrawl_enable_warnings(fluxcrawl_version_test)
add_test(NAME core.version COMMAND fluxcrawl_version_test)

foreach(case IN ITEMS help version invalid help_invalid seed missing extra_seed)
    add_test(NAME cli.${case}
        COMMAND "${CMAKE_COMMAND}"
            "-DFLUXCRAWL=$<TARGET_FILE:fluxcrawl>"
            "-DCASE=${case}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
    set_tests_properties(cli.${case} PROPERTIES TIMEOUT 10)
endforeach()
