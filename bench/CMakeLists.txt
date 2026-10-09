add_executable(stealpool_bench scaling_bench.cpp)
target_link_libraries(stealpool_bench PRIVATE stealpool benchmark::benchmark stealpool_warnings)
target_include_directories(stealpool_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Recorded in the benchmark JSON context next to Google Benchmark's own host information.
target_compile_definitions(stealpool_bench PRIVATE
  STEALPOOL_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  STEALPOOL_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

if(STEALPOOL_BUILD_TESTS)
  # A seconds-long smoke run on tiny inputs: checks that every benchmark runs and verifies
  # its result. It measures nothing; real runs are described in docs/BENCHMARKS.md.
  add_test(NAME bench.smoke
    COMMAND stealpool_bench --threads=1,2,3 --sort_size=200000 --tree_nodes=50000 --tree_work=4
            --spawn_depth=10 --benchmark_min_time=1x --benchmark_repetitions=2
            --benchmark_out=${CMAKE_CURRENT_BINARY_DIR}/smoke.json --benchmark_out_format=json)
  set_tests_properties(bench.smoke PROPERTIES LABELS "bench" TIMEOUT 300
    FAIL_REGULAR_EXPRESSION "ERROR OCCURRED" FIXTURES_SETUP bench_json)

  # The report script must understand what the benchmark binary writes.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME bench.report
      COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/scripts/bench_report.py
              ${CMAKE_CURRENT_BINARY_DIR}/smoke.json)
    set_tests_properties(bench.report PROPERTIES LABELS "bench" TIMEOUT 60 FIXTURES_REQUIRED bench_json
      PASS_REGULAR_EXPRESSION "\\| 3 \\| [0-9.]+ \\| [0-9.]+x \\| [0-9.]+ \\| [0-9.]+x \\| [0-9.]+ \\|")
  endif()
endif()
