# Bench.hpp lives alongside the benchmark sources.
function(jaf_add_benchmark name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE jaf::jaf)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

jaf_add_benchmark(ContextSwitchBench ContextSwitchBench.cpp)
jaf_add_benchmark(SchedulerBench      SchedulerBench.cpp)
jaf_add_benchmark(GraphBench          GraphBench.cpp)
