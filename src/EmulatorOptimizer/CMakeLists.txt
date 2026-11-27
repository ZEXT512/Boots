add_library(EmulatorOptimizer STATIC
    EmulatorOptimizer.cpp
    EmulatorOptimizer.h
)

target_include_directories(EmulatorOptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(EmulatorOptimizer PUBLIC
    common
    nlohmann_json::nlohmann_json
)

# GetProcessMemoryInfo (PSAPI) is used by analyze_performance().
target_link_libraries(EmulatorOptimizer PRIVATE
    psapi
)
