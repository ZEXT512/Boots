// BOOTFPS — WebView2 host window.
//
// The UI lives in src/UI/web (Cyber Sigilism frontend). This file owns the
// native window, the WebView2 runtime and the JSON bridge that connects the
// frontend to the real C++ modules (see Bridge.cpp).
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Bridge.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
    HWND g_hwnd = nullptr;
    Bridge* g_bridge = nullptr;
    ComPtr<ICoreWebView2Controller> g_controller;
    ComPtr<ICoreWebView2> g_webview;

    std::wstring executable_dir() {
        wchar_t path[MAX_PATH] = {};
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring full(path, length);
        const auto slash = full.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : full.substr(0, slash);
    }

    std::wstring local_app_data_dir() {
        wchar_t path[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
        return length ? std::wstring(path, length) : std::wstring(L"C:\\BOOTFPS");
    }

    std::string to_utf8(const std::wstring& text) {
        if (text.empty()) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::wstring to_wide(const std::string& text) {
        if (text.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            out.data(), needed);
        return out;
    }

    void resize_webview() {
        if (!g_controller) return;
        RECT bounds{};
        GetClientRect(g_hwnd, &bounds);
        g_controller->put_Bounds(bounds);
    }

    void wire_host_bridge() {
        EventRegistrationToken messageToken{};
        g_webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR raw = nullptr;
                    if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                        std::string message = to_utf8(raw);
                        CoTaskMemFree(raw);

                        // Low-level input hooks must be installed on the thread
                        // that pumps messages, so latency commands stay on the
                        // UI thread. Everything else is queued to the worker.
                        const bool synchronous =
                            message.find("\"input.latency.start\"") != std::string::npos ||
                            message.find("\"input.latency.stop\"") != std::string::npos;

                        if (!g_bridge) {
                            sender->PostWebMessageAsJson(
                                L"{\"id\":null,\"ok\":false,\"error\":\"bridge offline\"}");
                        } else if (synchronous) {
                            std::wstring response = to_wide(g_bridge->handle(message));
                            sender->PostWebMessageAsJson(response.c_str());
                        } else {
                            g_bridge->submit(message);
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &messageToken);

        EventRegistrationToken navigationToken{};
        g_webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    sender->PostWebMessageAsJson(L"{\"type\":\"ready\",\"app\":\"BOOTFPS\"}");
                    return S_OK;
                })
                .Get(),
            &navigationToken);
    }

    void initialize_webview(HWND hwnd) {
        std::wstring userData = local_app_data_dir() + L"\\BOOTFPS\\WebView2";
        std::error_code ec;
        std::filesystem::create_directories(userData, ec);

        auto environmentHandler =
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [hwnd](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(result) || !environment) {
                        MessageBoxW(hwnd,
                                    L"Unable to create the WebView2 environment.\n"
                                    L"Verify that the WebView2 Runtime is installed.",
                                    L"BOOTFPS", MB_ICONERROR | MB_OK);
                        return result;
                    }

                    environment->CreateCoreWebView2Controller(
                        hwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(result) || !controller) {
                                    MessageBoxW(hwnd,
                                                L"Unable to create the WebView2 controller.",
                                                L"BOOTFPS", MB_ICONERROR | MB_OK);
                                    return result;
                                }

                                g_controller = controller;
                                g_controller->get_CoreWebView2(&g_webview);

                                ComPtr<ICoreWebView2Controller2> controller2;
                                if (SUCCEEDED(g_controller.As(&controller2))) {
                                    COREWEBVIEW2_COLOR background{255, 5, 5, 5};
                                    controller2->put_DefaultBackgroundColor(background);
                                }

                                ComPtr<ICoreWebView2Settings> settings;
                                if (SUCCEEDED(g_webview->get_Settings(&settings))) {
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                                    settings->put_IsStatusBarEnabled(FALSE);
                                    settings->put_AreDevToolsEnabled(TRUE);
                                    settings->put_IsZoomControlEnabled(FALSE);
                                }

                                // Serve src/UI/web through a virtual https origin so
                                // relative CSS/JS paths and fetch() behave normally.
                                ComPtr<ICoreWebView2_3> webview3;
                                if (SUCCEEDED(g_webview.As(&webview3))) {
                                    std::wstring webRoot = executable_dir() + L"\\web";
                                    webview3->SetVirtualHostNameToFolderMapping(
                                        L"bootfps.local",
                                        webRoot.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    g_webview->Navigate(L"https://bootfps.local/index.html");
                                } else {
                                    MessageBoxW(hwnd,
                                                L"This WebView2 Runtime does not support folder mapping.\n"
                                                L"Please update the WebView2 Runtime.",
                                                L"BOOTFPS", MB_ICONERROR | MB_OK);
                                }

                                wire_host_bridge();
                                resize_webview();
                                return S_OK;
                            })
                            .Get());
                    return S_OK;
                });

        HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userData.c_str(), nullptr, environmentHandler.Get());

        if (FAILED(result)) {
            MessageBoxW(hwnd,
                        L"WebView2 initialization failed.\n"
                        L"Install the Microsoft Edge WebView2 Runtime and try again.",
                        L"BOOTFPS", MB_ICONERROR | MB_OK);
        }
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_SIZE:
                resize_webview();
                return 0;

            case WM_ERASEBKGND: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                RECT area{};
                GetClientRect(hwnd, &area);
                FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                return 1;
            }

            case WM_BOOTFPS_BRIDGE_RESULT: {
                std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
                if (payload && g_webview) {
                    std::wstring wide = to_wide(*payload);
                    g_webview->PostWebMessageAsJson(wide.c_str());
                }
                return 0;
            }

            case WM_DESTROY:
                if (g_bridge) g_bridge->stop();
                PostQuitMessage(0);
                return 0;

            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    SetProcessDPIAware();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        MessageBoxW(nullptr, L"COM initialization failed.", L"BOOTFPS", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = window_proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"BOOTFPS_MainWindow";

    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"Failed to register the BOOTFPS window class.",
                    L"BOOTFPS", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    g_hwnd = CreateWindowExW(
        0, windowClass.lpszClassName, L"BOOTFPS // CYBER SIGILISM",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, instance, nullptr);

    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Failed to create the BOOTFPS window.",
                    L"BOOTFPS", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    // The bridge owns CoreEngine and every module instance.
    Bridge bridge;
    g_bridge = &bridge;
    bridge.attach(g_hwnd);
    bridge.start();

    ShowWindow(g_hwnd, showCommand);
    UpdateWindow(g_hwnd);

    initialize_webview(g_hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    bridge.stop();
    g_bridge = nullptr;
    g_webview.Reset();
    g_controller.Reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
