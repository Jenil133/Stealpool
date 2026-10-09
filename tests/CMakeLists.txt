include(GoogleTest)

# stealpool_add_test(<name> [LABEL <label>])
function(stealpool_add_test name)
  cmake_parse_arguments(ARG "" "LABEL" "" ${ARGN})
  if(NOT ARG_LABEL)
    set(ARG_LABEL unit)
  endif()
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE stealpool GTest::gtest_main stealpool_warnings)
  # One CTest entry per test case, so failures are reported individually. Discovery at
  # test time (PRE_TEST) avoids running sanitizer-instrumented binaries during the build.
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "${ARG_LABEL}" TIMEOUT 600)
endfunction()

stealpool_add_test(deque_test)
stealpool_add_test(thread_pool_test)
stealpool_add_test(task_group_test)
stealpool_add_test(parallel_for_test)
stealpool_add_test(shutdown_test)
stealpool_add_test(task_graph_test)
stealpool_add_test(shared_queue_pool_test)
# The benchmark algorithms are checked for correctness on both pools.
target_include_directories(shared_queue_pool_test PRIVATE ${PROJECT_SOURCE_DIR}/bench)
# Longer randomized runs; part of the default suite, selectable with `ctest -L stress`
# (or skipped with `-LE stress`). STEALPOOL_STRESS_SEED / STEALPOOL_STRESS_SCALE in the
# environment pick the seed and the amount of work (see tests/stress_util.hpp).
stealpool_add_test(deque_stress_test LABEL stress)
stealpool_add_test(pool_stress_test LABEL stress)
