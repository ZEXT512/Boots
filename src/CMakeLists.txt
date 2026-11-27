# ── BOOTFPS Module Aggregation ───────────────────────────────────────────
#
# Every module is a real compiled target. The previously header-only
# HardwareMonitor / WindowsSystemAnalyzer / CoreEngine now have .cpp
# implementations so the UI can call them.
#
#   common                  INTERFACE  Logger + Result
#   WindowsSystemAnalyzer   STATIC     OS / hardware identity
#   HardwareMonitor         STATIC     live PDH sampler + PdhQueryWrapper
#   ProcessManager          STATIC     process scan / classify / terminate
#   InputLatencyOptimizer   STATIC     mouse, keyboard, USB, hooks
#   RegistryOptimizer       STATIC     curated registry tuning
#   EmulatorOptimizer       STATIC     MSI / BlueStacks profiles
#   PowerOptimizer          STATIC     power schemes / profiles
#   Benchmark               STATIC     FPS benchmark + reports
#   CoreEngine              STATIC     facade owning one of each module
# ─────────────────────────────────────────────────────────────────────────

# ── Header-only: common ─────────────────────────────────────────────────
add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ── WindowsSystemAnalyzer ───────────────────────────────────────────────
add_library(WindowsSystemAnalyzer STATIC
    WindowsSystemAnalyzer/SystemInfoProvider.cpp
    WindowsSystemAnalyzer/SystemInfoProvider.h
)
target_include_directories(WindowsSystemAnalyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/WindowsSystemAnalyzer)
target_link_libraries(WindowsSystemAnalyzer PUBLIC common)
target_link_libraries(WindowsSystemAnalyzer PRIVATE advapi32)

# ── HardwareMonitor (also owns PdhQueryWrapper) ─────────────────────────
add_library(HardwareMonitor STATIC
    HardwareMonitor/HardwareMonitor.cpp
    HardwareMonitor/HardwareMonitor.h
)
target_include_directories(HardwareMonitor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/HardwareMonitor)
target_link_libraries(HardwareMonitor PUBLIC common)
target_link_libraries(HardwareMonitor PRIVATE pdh)

# ── Compiled modules ────────────────────────────────────────────────────
add_subdirectory(ProcessManager)
add_subdirectory(InputLatencyOptimizer)
add_subdirectory(RegistryOptimizer)
add_subdirectory(EmulatorOptimizer)
add_subdirectory(PowerOptimizer)
add_subdirectory(Benchmark)

# ── CoreEngine facade ───────────────────────────────────────────────────
add_library(CoreEngine STATIC
    CoreEngine/CoreEngine.cpp
    CoreEngine/CoreEngine.h
)
target_include_directories(CoreEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/CoreEngine
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(CoreEngine PUBLIC common)
target_link_libraries(CoreEngine PRIVATE
    ProcessManager
    InputLatencyOptimizer
    RegistryOptimizer
    EmulatorOptimizer
    PowerOptimizer
    Benchmark
    HardwareMonitor
    WindowsSystemAnalyzer
)

# ── Aggregate INTERFACE library ─────────────────────────────────────────
# bootfps_core propagates all module dependencies to anything that links
# against it (the bridge, the UI executable, the tests).
add_library(bootfps_core INTERFACE)
target_link_libraries(bootfps_core INTERFACE
    common
    CoreEngine
    WindowsSystemAnalyzer
    HardwareMonitor
    ProcessManager
    InputLatencyOptimizer
    RegistryOptimizer
    EmulatorOptimizer
    PowerOptimizer
    Benchmark
)
target_include_directories(bootfps_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── UI executable ───────────────────────────────────────────────────────
add_subdirectory(UI)
