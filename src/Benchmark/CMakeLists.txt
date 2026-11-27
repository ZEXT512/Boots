add_library(Benchmark STATIC
    Benchmark.cpp
    Benchmark.h
)

target_include_directories(Benchmark PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Benchmark PUBLIC
    common
    HardwareMonitor
    nlohmann_json::nlohmann_json
)

# PDH for performance counters
target_link_libraries(Benchmark PRIVATE
    pdh
)
