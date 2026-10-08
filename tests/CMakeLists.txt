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
