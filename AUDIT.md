# BOOTFPS — Project Audit Report

Date: 2026-11-26
Root: `C:/Users/zext/Downloads/Boot fps`
Standard: C++20, MSVC (Visual Studio 2022, toolset 14.44), Windows x64

## 1. Complete project structure

```
Boot fps/
├── CMakeLists.txt                      # Root: C++20, FetchContent(nlohmann_json), add_subdirectory(src), bootfps_ui
├── AUDIT.md                            # This report
├── .vs/                                # Visual Studio workspace state (not source)
├── build/                              # Stale configure attempt (CMake 4.5.1)
│   ├── CMakeCache.txt
│   ├── CMakeFiles/
│   └── _deps/nlohmann_json-*           # Failed download (hash mismatch)
├── out/build/x64-Debug/                # Stale VS CMake configure (Debug, partial)
└── src/
    ├── CMakeLists.txt                  # Module aggregation + bootfps_core INTERFACE target
    ├── common/
    │   ├── Logger.h                    # Header-only singleton logger (file sink)
    │   └── Result.h                    # Header-only Result<T>/Result<void>
    ├── CoreEngine/
    │   └── CoreEngine.h                # Header-only facade (declarations only)
    ├── WindowsSystemAnalyzer/
    │   └── SystemInfoProvider.h        # Header-only declarations only
    ├── HardwareMonitor/
    │   └── HardwareMonitor.h           # PdhQueryWrapper + HardwareSnapshot + HardwareMonitor
    ├── ProcessManager/
    │   ├── CMakeLists.txt              # STATIC lib; psapi, pdh
    │   ├── ProcessInfo.h
    │   ├── ProcessManager.h/.cpp       # Full implementation present
    ├── InputLatencyOptimizer/
    │   ├── CMakeLists.txt              # STATIC lib; setupapi, winusb
    │   ├── InputLatency.h/.cpp         # Full implementation present
    ├── RegistryOptimizer/
    │   ├── CMakeLists.txt              # STATIC lib
    │   ├── RegistryOptimizer.h/.cpp    # Full implementation present
    ├── EmulatorOptimizer/
    │   ├── CMakeLists.txt              # STATIC lib
    │   ├── EmulatorOptimizer.h/.cpp    # Full implementation present
    ├── PowerOptimizer/
    │   ├── CMakeLists.txt              # STATIC lib; PowrProf, ole32
    │   ├── PowerOptimizer.h/.cpp       # Full implementation present
    ├── Benchmark/
    │   ├── CMakeLists.txt              # STATIC lib; pdh; links HardwareMonitor + json
    │   ├── Benchmark.h/.cpp            # Full implementation present (incl. PdhQueryWrapper methods)
    └── UI/
        ├── CMakeLists.txt              # WIN32 exe; bootfps_core + webview2; copies web/
        ├── main.cpp                    # WebView2 host window + native host bridge
        └── web/
            ├── index.html              # Cyber Sigilism dashboard
            ├── style.css
            └── app.js
tests/
├── CMakeLists.txt
└── bootfps_smoke.cpp                   # real smoke tests (CTest)
```

### Module classification (as configured in `src/CMakeLists.txt`)

| Target | Type | Sources | Notes |
|---|---|---|---|
| `common` | INTERFACE | Logger.h, Result.h | include dir `src/common` |
| `CoreEngine` | INTERFACE | CoreEngine.h | include dir `src/CoreEngine`; links `common` |
| `WindowsSystemAnalyzer` | INTERFACE | SystemInfoProvider.h | include dir `src/WindowsSystemAnalyzer` |
| `HardwareMonitor` | INTERFACE | HardwareMonitor.h | include dir `src/HardwareMonitor`; no .cpp |
| `ProcessManager` | STATIC | ProcessManager.cpp | + psapi, pdh |
| `InputLatencyOptimizer` | STATIC | InputLatency.cpp | + setupapi, winusb |
| `RegistryOptimizer` | STATIC | RegistryOptimizer.cpp | advapi32 implicit |
| `EmulatorOptimizer` | STATIC | EmulatorOptimizer.cpp | — |
| `PowerOptimizer` | STATIC | PowerOptimizer.cpp | + PowrProf, ole32 |
| `Benchmark` | STATIC | Benchmark.cpp | + pdh; links HardwareMonitor |
| `bootfps_core` | INTERFACE | — | aggregates all above + `src` include dir |
| `bootfps_ui` | WIN32 executable | src/UI/main.cpp | links `bootfps_core` |

### Frontend / WebView2 audit

**At audit time:** no frontend files existed, no WebView2 reference existed in CMake or C++, and no
WebView2 package was configured. The only design-bearing artifact was the Cyber-Sigilism-styled
HTML report in `PerformanceBenchmark::generate_html_report()` (palette `#050505` / `#F5F5F5` /
`#292929` / `#151515`), which was preserved verbatim.

**Follow-up UI repair (requested after the build audit):** the 6-line `WinMain` placeholder was
the reason the executable "opened and closed instantly". The UI was implemented as a real WebView2
host:

- `Microsoft.Web.WebView2` SDK **1.0.4370.26** is downloaded from NuGet at configure time
  (SHA256 pinned) and linked through the static loader `WebView2LoaderStatic.lib`.
- `src/UI/main.cpp` creates the native window, initializes WebView2, maps the local `web/`
  folder to `https://bootfps.local`, handles resize/background painting and exposes a
  `postMessage` host bridge (`ping` / `scan` / `ready`).
- `src/UI/web/` contains the Cyber Sigilism frontend (dashboard, module navigation, animated
  sigil, activity log, host status). The report palette remains unchanged.
- The WebView2 Runtime was already installed on the machine (`158.0.4370.33`).

**Conclusion:** problem #5 is now genuinely resolved — the dependency is explicit, versioned and
verified by an actual running WebView2 window.

## 2. Exact causes of build errors

### Confirmed by reproducing a fresh Release configure

1. **Wrong SHA256 for `nlohmann_json`** (`CMakeLists.txt:18`).
   - Declared: `d6c65aca6b1571f05...80dbb7c4767`
   - Actual v3.11.3 `json.tar.xz`: `d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d`
   - Result: FetchContent download retried 6×, then `CMake Error ... Each download failed!`.
   - Fix applied: hash corrected. Configure then completed successfully.

### Problems #1 and #2 (module dirs / add_subdirectory) — already resolved in the tree

All six compiled modules have existing directories and valid `CMakeLists.txt` files; all six
`add_subdirectory()` calls in `src/CMakeLists.txt:35-40` resolve. Configure confirms this.
The stale `out/build/x64-Debug` tree only contains two configured module folders because it is an
interrupted previous attempt, not evidence of a current CMake error.

### Confirmed by source inspection (compile verification in progress)

2. **`Logger::log()` accepts `std::string_view`, but every module calls it with wide strings**
   (`L"..."`, `std::wstring`) — e.g. `ProcessManager.cpp:51`, `RegistryOptimizer.cpp:56`,
   `PowerOptimizer.cpp:47`, `Benchmark.cpp:15`, `EmulatorOptimizer.cpp:58`,
   `InputLatencyOptimizer.cpp:44`. No implicit conversion exists → compile error.

3. **`Benchmark.cpp` uses `PdhQueryWrapper` without including `HardwareMonitor.h`**
   (`Benchmark.h:79-82`, `Benchmark.cpp:27-56`). Additionally `Benchmark.cpp:72` accesses
   private member `PdhQueryWrapper::_counters` → compile error.

4. **`InputLatencyOptimizer` hook callbacks reference members that do not exist / are private**
   (`InputLatency.cpp:212-216,228-233`): `optimizer->_frequency` is not declared in
   `InputLatency.h` (only `_last_timestamp` is), and `_latency_samples`, `_last_timestamp` are
   private while the callbacks are free functions → compile error.

5. **`PowerOptimizer.cpp:87` calls `PowerEnumerate` with the wrong signature**
   (passes `&guid` where `POWER_DATA_ACCESSOR` is expected, `&guidSize` as index, `&index` as
   buffer) → compile error.

6. **`HardwareMonitor.h` uses `std::thread` without `<thread>`** (line 80) and declares
   `HardwareMonitor` methods with no implementation (no `HardwareMonitor.cpp` exists).
   `PdhQueryWrapper` methods are implemented in `Benchmark.cpp` instead.

7. **`CoreEngine.h` includes `"common/Result.h"` / `"common/Logger.h"`** but the `CoreEngine`
   target only propagates `src/CoreEngine` and `src/common` include dirs, not `src/`; it also uses
   `std::function` without `<functional>` and declares `CPUOptimizer::CpuOptimizer` (a class that
   does not exist anywhere). Declared methods have no definitions.

8. **`WindowsSystemAnalyzer::SystemInfoProvider`** declares methods with no implementation.

9. **Missing standard includes** (e.g. `EmulatorOptimizer.cpp` uses `std::this_thread::sleep_for`
   without `<thread>`; `Benchmark.cpp` uses `std::floor`/`std::abs` without `<cmath>` and
   `strftime` without `<ctime>`). Some may be satisfied transitively; compile will decide.

### Additional errors found in the second compile pass (all confirmed by MSVC output)

- `EmulatorOptimizer.cpp`: `PROCESS_MEMORY_COUNTERS_EX` used without `<psapi.h>`; `psapi.lib`
  was not linked by the target.
- `EmulatorOptimizer.cpp` / `ProcessManager.cpp`: `std::min`/`std::max` broken by the Windows
  `min`/`max` macros (`NOMINMAX` was not defined globally).
- `InputLatency.cpp`: `GUID_DEVINTERFACE_HID` requires `<hidclass.h>` and `hid.lib`.
- `PowerOptimizer.cpp`: `PowerEnumerate` buffer must be cast to `UCHAR*`; `PowerDuplicateScheme`
  returns a `GUID**` allocated with `LocalAlloc`.

## 3. Repairs applied

1. Corrected the `nlohmann_json` SHA256; Release configuration retained.
2. `Logger` now accepts both `std::string_view` and `std::wstring_view` (UTF-8 conversion), so all
   existing wide-string call sites work unchanged; added the missing `<sstream>` include and a
   `ProgramData` fallback.
3. `Benchmark.h` includes `HardwareMonitor.h`; added `PdhQueryWrapper::counter_count()` and used it
   instead of accessing private `_counters`. Fixed an extra `)` typo in the HTML report generator.
   The Cyber Sigilism palette and report layout were left unchanged.
4. `InputLatencyOptimizer`: replaced the nonexistent `SPI_GETMOUSEACCELERATE` /
   `SPI_SETMOUSEACCELERATE` with the documented `SPI_GETMOUSE` / `SPI_SETMOUSE` third array slot;
   added `_frequency`, a public `record_latency_event()` used by the hook callbacks, a mutex for
   samples, and proper cleanup of both hook handles; added `hid.lib` to the target.
5. `PowerOptimizer`: fixed `PowerEnumerate` and `PowerDuplicateScheme` signatures and replaced
   `malloc`/`free` with `LocalFree` for API-allocated buffers.
6. Header self-containment: `<unordered_map>`, `<thread>`, `<cmath>`, `<ctime>`, `<functional>`;
   relative common-header includes in `CoreEngine.h`; removed the nonexistent `CPUOptimizer`
   forward declaration and incomplete-type members from `CoreEngine.h`.
7. Global `NOMINMAX` define; `psapi.lib` added to EmulatorOptimizer.
8. Added a real smoke test (`tests/bootfps_smoke.cpp`) covering `Result`, JSON/HTML reports,
   comparison/rollback logic and the live PDH wrapper. No empty placeholder modules added.
9. Replaced the placeholder `WinMain` with a real WebView2 host window; added
   `src/UI/CMakeLists.txt`, the Cyber Sigilism frontend under `src/UI/web/`, the pinned WebView2
   SDK download, and the `webview2` INTERFACE target. `bootfps_ui` is now built from `src/UI`
   instead of being defined inline in the root CMakeLists.

## 4. Verification

- Configure: `cmake -S . -B build/Release -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release` — success.
- Build: `cmake --build build/Release --config Release` — success, zero errors.
- Executable generated: `build/Release/src/UI/Release/bootfps_ui.exe` (76,800 bytes).
- Executable executed: window opens with title `BOOTFPS // CYBER SIGILISM`, stays alive, and
  spawns WebView2 render processes (`Chrome_WidgetWin_1`, `Chrome_RenderWidgetHostHWND`,
  `Intermediate D3D Window` confirmed as child windows).
- WebView2 user data created at `%LOCALAPPDATA%\BOOTFPS\WebView2\EBWebView`.
- Test: `ctest --test-dir build/Release -C Release --output-on-failure` — 1/1 passed (11 assertions,
  including the Cyber Sigilism palette check).
