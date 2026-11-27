add_library(ProcessManager STATIC
    ProcessManager.cpp
    ProcessManager.h
    ProcessInfo.h
)

target_include_directories(ProcessManager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ProcessManager PUBLIC
    common
    nlohmann_json::nlohmann_json
)

# System libraries needed by ProcessManager
target_link_libraries(ProcessManager PRIVATE
    psapi
    pdh
)
