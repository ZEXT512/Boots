# ── BOOTFPS UI ───────────────────────────────────────────────────────────
# The bridge is a library so the UI and the tests share exactly the same
# command surface.
add_library(bootfps_bridge STATIC
    Bridge.cpp
    Bridge.h
)
target_include_directories(bootfps_bridge PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bootfps_bridge PUBLIC
    bootfps_core
    nlohmann_json::nlohmann_json
)
target_link_libraries(bootfps_bridge PRIVATE
    ole32
)

add_executable(bootfps_ui WIN32 main.cpp)

target_link_libraries(bootfps_ui PRIVATE bootfps_bridge webview2)

# Ship the Cyber Sigilism frontend next to the executable.
add_custom_command(TARGET bootfps_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:bootfps_ui>/web"
    COMMENT "Copying BOOTFPS frontend assets"
)
