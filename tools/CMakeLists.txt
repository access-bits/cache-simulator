# Trace generation and inspection tools. Not part of the library.
add_executable(gen_trace gen_trace.cpp)
target_link_libraries(gen_trace PRIVATE cachesim_core cachesim_flags)

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE cachesim_core cachesim_flags)

add_executable(replay_bench replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE cachesim_core cachesim_flags)
