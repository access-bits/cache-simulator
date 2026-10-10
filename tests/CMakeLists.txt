# Each test is its own executable so that a crash in one does not hide the
# others, and so ctest can run them in parallel.
function(cachesim_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE cachesim_core cachesim_flags)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

cachesim_add_test(datastructures_test)
cachesim_add_test(cache_test)
cachesim_add_test(policy_test)
cachesim_add_test(replay_test)
