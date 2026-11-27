add_library(RegistryOptimizer STATIC
    RegistryOptimizer.cpp
    RegistryOptimizer.h
)

target_include_directories(RegistryOptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(RegistryOptimizer PUBLIC
    common
    nlohmann_json::nlohmann_json
)

# Registry API is part of advapi32 (linked by default on MSVC)
