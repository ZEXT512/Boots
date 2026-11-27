cmake_minimum_required(VERSION 3.20)

project(BOOTFPS LANGUAGES CXX)

# ── C++20 ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows headers define min/max macros that break std::min/std::max in the modules.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# IDE folders
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Third-party: nlohmann_json (centralized) ─────────────────────────────
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── Third-party: WebView2 SDK (NuGet package, static loader) ─────────────
set(WEBVIEW2_VERSION "1.0.4370.26")
set(WEBVIEW2_DIR "${CMAKE_BINARY_DIR}/_deps/webview2")
set(WEBVIEW2_HEADER "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
set(WEBVIEW2_STATIC_LIB "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${WEBVIEW2_HEADER}" OR NOT EXISTS "${WEBVIEW2_STATIC_LIB}")
    set(WEBVIEW2_NUPKG "${WEBVIEW2_DIR}/webview2.nupkg")
    message(STATUS "Downloading Microsoft.Web.WebView2 ${WEBVIEW2_VERSION}...")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
        "${WEBVIEW2_NUPKG}"
        EXPECTED_HASH SHA256=1C0DACAC84B70109B3FD42F90C53A0BA942ABC3A87590D7A4E0CCD86E4A06CF4
        TLS_VERIFY ON
    )
    file(ARCHIVE_EXTRACT INPUT "${WEBVIEW2_NUPKG}" DESTINATION "${WEBVIEW2_DIR}")
endif()

add_library(webview2 INTERFACE)
target_include_directories(webview2 INTERFACE "${WEBVIEW2_DIR}/build/native/include")
target_link_libraries(webview2 INTERFACE
    "${WEBVIEW2_STATIC_LIB}"
    advapi32
    ole32
    oleaut32
    shell32
    shlwapi
    user32
    version
)

# ── Modules ──────────────────────────────────────────────────────────────
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
