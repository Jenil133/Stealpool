foreach(example fib quicksort tree_sum pipeline_graph)
  add_executable(example_${example} ${example}.cpp)
  target_link_libraries(example_${example} PRIVATE stealpool stealpool_warnings)
endforeach()

if(STEALPOOL_BUILD_TESTS)
  # Each example verifies its own result and exits non-zero on mismatch.
  add_test(NAME example.fib COMMAND example_fib 27 1)
  add_test(NAME example.fib_many COMMAND example_fib 27)
  add_test(NAME example.quicksort COMMAND example_quicksort 500000 2)
  add_test(NAME example.tree_sum COMMAND example_tree_sum 200000 3)
  add_test(NAME example.pipeline_graph COMMAND example_pipeline_graph 200000 12 4 3)
  set_tests_properties(example.fib example.fib_many example.quicksort example.tree_sum
    example.pipeline_graph PROPERTIES LABELS "example" TIMEOUT 300)
endif()
