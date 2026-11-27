add_executable(bootfps_smoke_tests bootfps_smoke.cpp)
target_include_directories(bootfps_smoke_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bootfps_smoke_tests PRIVATE Benchmark)

add_test(NAME bootfps_smoke COMMAND bootfps_smoke_tests)

add_executable(bootfps_bridge_tests bridge_smoke.cpp)
target_include_directories(bootfps_bridge_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bootfps_bridge_tests PRIVATE bootfps_bridge)

add_test(NAME bootfps_bridge COMMAND bootfps_bridge_tests)
