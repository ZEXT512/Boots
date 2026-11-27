add_library(InputLatencyOptimizer STATIC
    InputLatency.cpp
    InputLatency.h
)

target_include_directories(InputLatencyOptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(InputLatencyOptimizer PUBLIC
    common
    nlohmann_json::nlohmann_json
)

# System libraries needed by InputLatencyOptimizer
target_link_libraries(InputLatencyOptimizer PRIVATE
    setupapi
    hid
    winusb
)
