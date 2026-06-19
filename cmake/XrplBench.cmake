option(
    bench
    "Build the pqc microbench target (requires the benchmark Conan dependency, enabled with -o bench=True)."
    OFF
)

if(bench)
    include(deps/GoogleBenchmark)

    add_executable(xrpl.bench.pqc src/bench/pqc_bench.cpp)

    target_link_libraries(
        xrpl.bench.pqc
        PRIVATE
            Xrpl::opts
            Xrpl::libs
            Xrpl::boost
            xrpl.libxrpl
            benchmark::benchmark
    )

    set_target_properties(
        xrpl.bench.pqc
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
