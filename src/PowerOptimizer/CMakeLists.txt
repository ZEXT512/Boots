add_library(PowerOptimizer STATIC
    PowerOptimizer.cpp
    PowerOptimizer.h
)

target_include_directories(PowerOptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(PowerOptimizer PUBLIC
    common
    nlohmann_json::nlohmann_json
)

# PowrProf.lib for power management APIs
target_link_libraries(PowerOptimizer PRIVATE
    PowrProf
    ole32
    shlwapi
)
