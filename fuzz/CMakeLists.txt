foreach(parser IN ITEMS url resolve robots checkpoint)
    add_executable(fluxcrawl_fuzz_${parser} ${parser}_fuzz.cpp)
    target_link_libraries(fluxcrawl_fuzz_${parser} PRIVATE FluxCrawl::core)
    target_link_options(fluxcrawl_fuzz_${parser} PRIVATE -fsanitize=fuzzer)
    fluxcrawl_enable_warnings(fluxcrawl_fuzz_${parser})
    file(COPY corpus/${parser}/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/corpus/${parser})
    if(BUILD_TESTING)
        add_test(NAME fuzz.${parser} COMMAND fluxcrawl_fuzz_${parser}
            -max_total_time=10 -max_len=16384 -timeout=5 -rss_limit_mb=512
            ${CMAKE_CURRENT_BINARY_DIR}/corpus/${parser}/)
        set_tests_properties(fuzz.${parser} PROPERTIES LABELS "fuzz;fuzz-smoke" TIMEOUT 30
            ENVIRONMENT "ASAN_OPTIONS=quarantine_size_mb=32")
    endif()
endforeach()
