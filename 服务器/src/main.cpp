#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroServer.h"

#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ServerDemo.MainWindow";
constexpr int kButtonCreateServer = 1001;
constexpr int kToolbarHeight = 56;
constexpr int kServerPort = 8888;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_create_server_button = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefServer> g_server;
CefRefPtr<FBroHsServerHandle> g_server_handler;
bool g_fbro_ready = false;
bool g_server_create_requested = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;

std::filesystem::path ExeDir() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

std::string ToSystemAnsi(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromCefString(const CefString& value) {
    return value.ToWString();
}

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"server-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

bool HasLiveResources() {
    return g_browser != nullptr || g_server != nullptr || g_server_create_requested;
}

void MaybeDestroyAfterClose() {
    if (!g_close_requested || g_destroying_window || HasLiveResources()) {
        return;
    }

    g_destroying_window = true;
    if (g_main_window) {
        KillTimer(g_main_window, kCloseFallbackTimer);
        DestroyWindow(g_main_window);
    }
    FBroQuitMessageLoop();
    PostQuitMessage(0);
}

void RequestAppClose(HWND hwnd) {
    if (g_destroying_window) {
        return;
    }

    g_close_requested = true;
    ShowWindow(hwnd, SW_HIDE);
    SetTimer(hwnd, kCloseFallbackTimer, kCloseFallbackMs, nullptr);
    if (g_server) {
        FBroHsServer_Shutdown(g_server);
    } else {
        g_server_create_requested = false;
    }
    if (g_browser) {
        FBroHsBrowserHost_CloseBrowser(g_browser, true);
    }
    if (!g_fbro_shutdown_started) {
        g_fbro_shutdown_started = true;
        FBroShutdown(FALSE);
    }
    MaybeDestroyAfterClose();
}

void Notify(const std::wstring& text) {
    LogLine(text);
    MessageBoxW(g_main_window, text.c_str(),
        L"\u670d\u52a1\u5668", MB_ICONINFORMATION);
}

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    if (g_create_server_button) {
        MoveWindow(g_create_server_button, 16, 12, 140, 32, TRUE);
    }
    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, kToolbarHeight, width,
            max(1, height - kToolbarHeight), TRUE);
    }
    if (g_browser) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd && g_browser_host) {
            RECT host_rc{};
            GetClientRect(g_browser_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                host_rc.right - host_rc.left,
                host_rc.bottom - host_rc.top,
                TRUE);
        }
    }
}

class ServerEvent final : public FBroHsServerHandle {
public:
    ServerEvent() {
        type_ = ServerHandleType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) {
            FBroMallocManger_Free(ptr);
        }
    }

    void OnServerCreated(CefRefPtr<CefServer> server) override {
        g_server = server;
        LogLine(L"\u670d\u52a1\u5668\u521b\u5efa\u6210\u529f: 127.0.0.1:" +
            std::to_wstring(kServerPort));
    }

    void OnServerDestroyed(CefRefPtr<CefServer>) override {
        LogLine(L"Server destroyed.");
        g_server = nullptr;
        g_server_create_requested = false;
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgMaybeDestroy, 0, 0);
        }
    }

    void OnClientConnected(CefRefPtr<CefServer>, int connection_id) override {
        LogLine(L"Client connected: " + std::to_wstring(connection_id));
    }

    void OnClientDisconnected(CefRefPtr<CefServer>, int connection_id) override {
        LogLine(L"Client disconnected: " + std::to_wstring(connection_id));
    }

    void OnHttpRequest(CefRefPtr<CefServer> server,
                       int connection_id,
                       const CefString& client_address,
                       CefRefPtr<CefRequest>) override {
        LogLine(L"HTTP request from " + FromCefString(client_address) +
            L", connection=" + std::to_wstring(connection_id));
        static const char body[] =
            "<!doctype html><meta charset=\"utf-8\">"
            "<title>FBro ServerDemo</title>"
            "<body>FBro native C++ server is running.</body>";
        FBroHsServer_SendHttp200Response(server, connection_id,
            CefString("text/html; charset=utf-8"), body,
            static_cast<int>(sizeof(body) - 1));
    }

    void OnWebSocketRequest(CefRefPtr<CefServer>,
                            int connection_id,
                            const CefString& client_address,
                            CefRefPtr<CefRequest>,
                            CefRefPtr<CefCallback>) override {
        LogLine(L"WebSocket request from " + FromCefString(client_address) +
            L", connection=" + std::to_wstring(connection_id));
    }

    void OnWebSocketConnected(CefRefPtr<CefServer>, int connection_id) override {
        LogLine(L"WebSocket connected: " + std::to_wstring(connection_id));
    }

    void OnWebSocketMessage(CefRefPtr<CefServer> server,
                            int connection_id,
                            const void* data,
                            size_t data_size) override {
        LogLine(L"WebSocket message: connection=" +
            std::to_wstring(connection_id) + L", size=" +
            std::to_wstring(data_size));
        if (server && data && data_size > 0) {
            FBroHsServer_SendWebSocketMessage(server, connection_id, data,
                static_cast<int>(data_size));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(ServerEvent);
};

void CreateServer() {
    if (!g_fbro_ready) {
        Notify(L"FBro is not ready.");
        return;
    }
    if (g_server_create_requested || (g_server && FBroHsServer_IsRunning(g_server))) {
        Notify(L"\u670d\u52a1\u5668\u5df2\u5728\u8fd0\u884c: 127.0.0.1:" +
            std::to_wstring(kServerPort));
        return;
    }

    g_server_create_requested = true;
    g_server_handler = new ServerEvent();
    FBroHsServer_CreateServer(CefString("127.0.0.1"), kServerPort, 100,
        g_server_handler);
    LogLine(L"Create server submitted: 127.0.0.1:" +
        std::to_wstring(kServerPort));
}

class BrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        LogLine(L"Embedded chat-test browser created.");
        LayoutChildren();
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser == g_browser) {
            g_browser = nullptr;
        }
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgMaybeDestroy, 0, 0);
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>,
                   CefRefPtr<CefFrame> frame,
                   int http_status_code) override {
        if (frame && frame->IsMain()) {
            LogLine(L"Chat-test page load ended, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

void CreateEmbeddedBrowser() {
    if (!g_fbro_ready || !g_browser_host || g_browser) return;

    RECT rc{};
    GetClientRect(g_browser_host, &rc);

    E_WINDOWS_INFO window_info{};
    window_info.is_null = FALSE;
    window_info.parent_window = g_browser_host;
    window_info.x = 0;
    window_info.y = 0;
    window_info.width = max(1, rc.right - rc.left);
    window_info.height = max(1, rc.bottom - rc.top);
    window_info.style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    window_info.ex_style = 0;
    window_info.windowless_rendering_enabled = FALSE;
    window_info.shared_texture_enabled = FALSE;
    window_info.external_begin_frame_enabled = FALSE;

    FBroBrowserSetting browser_setting{};
    browser_setting.is_null = TRUE;
    browser_setting.background_color = 0x00FFFFFF;

    CefRefPtr<BrowserEvent> event = new BrowserEvent();
    CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
    extra->SetString("flag", "ServerDemo");

    const BOOL ok = FBroHsCreate(CefString("http://coolaf.com/tool/chattest"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("ServerDemo"));
    if (!ok) {
        LogLine(L"FBroHsCreate failed.");
    }
}

class InitEvent final : public FBroHsInitEvent {
public:
    void OnContextInitialized() override {
        g_fbro_ready = true;
        LogLine(L"FBro context initialized.");
        CreateEmbeddedBrowser();
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
};

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_create_server_button = CreateWindowExW(0, L"BUTTON",
            L"\u521b\u5efa\u670d\u52a1\u5668",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 12, 140, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCreateServer)),
            GetModuleHandleW(nullptr), nullptr);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kButtonCreateServer) {
            CreateServer();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case WM_TIMER:
        if (wparam == kCloseFallbackTimer && g_close_requested) {
            LogLine(L"Close fallback timer fired.");
            g_browser = nullptr;
            g_server = nullptr;
            g_server_create_requested = false;
            MaybeDestroyAfterClose();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case kMsgMaybeDestroy:
        MaybeDestroyAfterClose();
        return 0;
    case WM_CLOSE:
        RequestAppClose(hwnd);
        return 0;
    case WM_DESTROY:
        FBroQuitMessageLoop();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

bool CreateMainWindow(HINSTANCE instance, int show_cmd) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&wc)) return false;

    g_main_window = CreateWindowExW(WS_EX_APPWINDOW, kMainWindowClass,
        L"\u670d\u52a1\u5668",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        nullptr, nullptr, instance, nullptr);
    if (!g_main_window) return false;

    ShowWindow(g_main_window, show_cmd);
    UpdateWindow(g_main_window);
    return true;
}

bool InitFbro() {
    const auto app_dir = ExeDir();
    const auto subprocess = app_dir / L"FBroSubprocess.exe";
    const auto cache_dir = app_dir / L"CacheData" / L"GlobalData";
    const auto log_file = app_dir / L"debug.log";
    const auto locales_dir = app_dir / L"locales";

    const auto app_dir_ansi = ToSystemAnsi(app_dir.wstring());
    const auto subprocess_ansi = ToSystemAnsi(subprocess.wstring());
    const auto cache_ansi = ToSystemAnsi(cache_dir.wstring());
    const auto log_ansi = ToSystemAnsi(log_file.wstring());
    const auto locales_ansi = ToSystemAnsi(locales_dir.wstring());

    FBroHsSetProcessDPI(0);
    FBroSetV8DefaultsHeapSize(4, 2048);

    FBroInitSettings settings{};
    settings.no_sandbox = TRUE;
    settings.browser_subprocess_path = const_cast<char*>(subprocess_ansi.c_str());
    settings.multi_threaded_message_loop = FALSE;
    settings.external_message_pump = FALSE;
    settings.windowless_rendering_enabled = FALSE;
    settings.command_line_args_disabled = FALSE;
    settings.cache_path = const_cast<char*>(cache_ansi.c_str());
    settings.persist_session_cookies = TRUE;
    settings.locale = const_cast<char*>("zh-CN");
    settings.accept_language_list = const_cast<char*>("zh-CN,zh,en");
    settings.log_file = const_cast<char*>(log_ansi.c_str());
    settings.log_severity = LOGSEVERITY_DEFAULT;
    settings.resources_dir_path = const_cast<char*>(app_dir_ansi.c_str());
    settings.locales_dir_path = const_cast<char*>(locales_ansi.c_str());
    settings.enable_auto_multiple = TRUE;

    CefRefPtr<InitEvent> init_event = new InitEvent();
    return FBroHsInitPro(&settings, init_event, 1024) == TRUE;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_cmd) {
    setlocale(LC_CTYPE, "");
    SetCurrentDirectoryW(ExeDir().c_str());

    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed.", L"ServerDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"ServerDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"ServerDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    FBroRunMessageLoop();
    if (!g_fbro_shutdown_started) {
        FBroShutdown(FALSE);
    }
    WSACleanup();
    if (SUCCEEDED(co_result)) CoUninitialize();
    return 0;
}
