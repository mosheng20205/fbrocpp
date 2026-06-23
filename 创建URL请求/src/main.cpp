#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsBaseEvent.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroRequest.h"
#include "FBroRequestContext.h"

#include <algorithm>
#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"URLRequestDemo.MainWindow";
constexpr int kButtonGlobalRequest = 1001;
constexpr int kButtonFrameRequest = 1002;
constexpr int kToolbarHeight = 64;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT kMsgAppendLog = WM_APP + 302;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_global_button = nullptr;
HWND g_frame_button = nullptr;
HWND g_status_box = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
std::vector<CefRefPtr<CefURLRequestClient>> g_pending_requests;
bool g_fbro_ready = false;
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
    std::ofstream out(ExeDir() / L"url-request-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

int MaxInt(int a, int b) {
    return a > b ? a : b;
}

int MinInt(int a, int b) {
    return a < b ? a : b;
}

struct LogMessage {
    std::wstring text;
};

void AppendStatusDirect(const std::wstring& text) {
    LogLine(text);
    if (!g_status_box) return;

    const int len = GetWindowTextLengthW(g_status_box);
    SendMessageW(g_status_box, EM_SETSEL, len, len);
    SendMessageW(g_status_box, EM_REPLACESEL, FALSE,
        reinterpret_cast<LPARAM>((text + L"\r\n").c_str()));
}

void PostStatus(std::wstring text) {
    auto* message = new LogMessage{std::move(text)};
    if (g_main_window) {
        PostMessageW(g_main_window, kMsgAppendLog, 0,
            reinterpret_cast<LPARAM>(message));
    } else {
        delete message;
    }
}

bool HasLiveResources() {
    return g_browser != nullptr;
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
    if (g_browser) {
        FBroHsBrowserHost_CloseBrowser(g_browser, true);
    }
    if (!g_fbro_shutdown_started) {
        g_fbro_shutdown_started = true;
        FBroShutdown(FALSE);
    }
    MaybeDestroyAfterClose();
}

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    if (g_global_button) {
        MoveWindow(g_global_button, 16, 16, 180, 32, TRUE);
    }
    if (g_frame_button) {
        MoveWindow(g_frame_button, 208, 16, 180, 32, TRUE);
    }
    if (g_status_box) {
        MoveWindow(g_status_box, MaxInt(400, width - 500), 8,
            MinInt(480, MaxInt(1, width - 420)), 48, TRUE);
    }
    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, kToolbarHeight, width,
            MaxInt(1, height - kToolbarHeight), TRUE);
    }
    if (g_browser) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd && g_browser_host) {
            RECT host_rc{};
            GetClientRect(g_browser_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                MaxInt(1, host_rc.right - host_rc.left),
                MaxInt(1, host_rc.bottom - host_rc.top),
                TRUE);
        }
    }
}

std::wstring RequestStatusName(int status) {
    switch (status) {
    case 0: return L"UNKNOWN";
    case 1: return L"SUCCESS";
    case 2: return L"IO_PENDING";
    case 3: return L"CANCELED";
    case 4: return L"FAILED";
    default: return L"STATUS_" + std::to_wstring(status);
    }
}

std::wstring DescribeRequest(CefRefPtr<CefURLRequest> request) {
    if (!request) return L"request=null";

    std::wostringstream out;
    const int status = request->GetRequestStatus();
    out << L"status=" << RequestStatusName(status);

    CefRefPtr<CefRequest> inner = request->GetRequest();
    if (inner) {
        out << L", url=" << FromCefString(inner->GetURL());
    }

    CefRefPtr<CefResponse> response = request->GetResponse();
    if (response) {
        out << L", http=" << response->GetStatus()
            << L", mime=" << FromCefString(response->GetMimeType());
    }

    const int error = request->GetRequestError();
    if (error != 0) {
        out << L", error=" << error;
    }
    return out.str();
}

std::wstring DecodeResponseText(const std::string& data) {
    if (data.empty()) return {};

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        data.data(), static_cast<int>(data.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        code_page = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(code_page, flags,
            data.data(), static_cast<int>(data.size()), nullptr, 0);
    }
    if (size <= 0) {
        return L"<binary or undecodable response body>";
    }

    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(code_page, flags,
        data.data(), static_cast<int>(data.size()), text.data(), size);
    return text;
}

class URLRequestClient final : public CefURLRequestClient {
public:
    explicit URLRequestClient(std::wstring label)
        : label_(std::move(label)) {}

    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        PostStatus(label_ + L" complete, " +
            L", " + DescribeRequest(request));
        PostStatus(label_ + L" response body:\r\n" +
            DecodeResponseText(response_body_));
        ReleaseSelf();
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest>,
                          int64_t current,
                          int64_t total) override {
        PostStatus(label_ + L" upload " + std::to_wstring(current) +
            L"/" + std::to_wstring(total));
    }

    void OnDownloadProgress(CefRefPtr<CefURLRequest>,
                            int64_t current,
                            int64_t total) override {
        PostStatus(label_ + L" progress " + std::to_wstring(current) +
            L"/" + std::to_wstring(total));
    }

    void OnDownloadData(CefRefPtr<CefURLRequest> request,
                        const void* data,
                        size_t data_length) override {
        if (data && data_length > 0) {
            const auto* bytes = static_cast<const char*>(data);
            response_body_.append(bytes, bytes + data_length);
        }

        PostStatus(label_ + L" data bytes=" + std::to_wstring(data_length) +
            L", total=" + std::to_wstring(response_body_.size()) +
            L", " + DescribeRequest(request));
    }

    bool GetAuthCredentials(bool,
                            const CefString&,
                            int,
                            const CefString&,
                            const CefString&,
                            CefRefPtr<CefAuthCallback>) override {
        return false;
    }

private:
    void ReleaseSelf() {
        if (!g_main_window) return;
        PostMessageW(g_main_window, kMsgAppendLog, 1, reinterpret_cast<LPARAM>(this));
    }

    std::wstring label_;
    std::string response_body_;
    IMPLEMENT_REFCOUNTING(URLRequestClient);
};

CefRefPtr<CefRequest> CreateGetRequest() {
    CefRefPtr<CefRequest> request = CefRequest::Create();
    FBroHsRequest_SetURL(request, CefString(L"https://api.fbrowser.site:8443/"));
    FBroHsRequest_SetMethod(request, CefString(L"GET"));
    return request;
}

void TrackRequestClient(CefRefPtr<CefURLRequestClient> client) {
    g_pending_requests.push_back(client);
}

void RemoveRequestClient(CefURLRequestClient* client) {
    g_pending_requests.erase(std::remove_if(g_pending_requests.begin(),
        g_pending_requests.end(),
        [client](const CefRefPtr<CefURLRequestClient>& item) {
            return item.get() == client;
        }), g_pending_requests.end());
}

void CreateGlobalURLRequest() {
    CefRefPtr<CefRequest> request = CreateGetRequest();
    CefRefPtr<URLRequestClient> client =
        new URLRequestClient(L"\u5168\u5c40URL\u8bf7\u6c42");
    TrackRequestClient(client);
    AppendStatusDirect(L"\u521b\u5efa\u5168\u5c40URL\u8bf7\u6c42");
    CefURLRequest::Create(request, client, nullptr);
}

void CreateFrameURLRequest() {
    if (!g_browser) {
        MessageBoxW(g_main_window, L"Browser is not ready.", L"URLRequestDemo",
            MB_ICONINFORMATION);
        return;
    }

    CefRefPtr<CefRequestContext> context =
        FBroHsBrowserHost_GetRequestContext(g_browser);
    if (!context) {
        MessageBoxW(g_main_window, L"Browser request context is not ready.", L"URLRequestDemo",
            MB_ICONINFORMATION);
        return;
    }

    CefRefPtr<CefRequest> request = CreateGetRequest();
    CefRefPtr<URLRequestClient> client =
        new URLRequestClient(L"\u6846\u67b6URL\u8bf7\u6c42");
    TrackRequestClient(client);
    AppendStatusDirect(L"\u521b\u5efa\u6846\u67b6URL\u8bf7\u6c42");
    CefURLRequest::Create(request, client, context);
}

class BrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        LogLine(L"Embedded Baidu browser created.");
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
            LogLine(L"Baidu page load ended, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

void CreateEmbeddedBaiduBrowser() {
    if (!g_fbro_ready || !g_browser_host || g_browser) {
        return;
    }

    RECT rc{};
    GetClientRect(g_browser_host, &rc);

    E_WINDOWS_INFO window_info{};
    window_info.is_null = FALSE;
    window_info.parent_window = g_browser_host;
    window_info.x = 0;
    window_info.y = 0;
    window_info.width = MaxInt(1, rc.right - rc.left);
    window_info.height = MaxInt(1, rc.bottom - rc.top);
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
    extra->SetString("flag", "URLRequestDemo");

    const BOOL ok = FBroHsCreate(CefString("https://www.baidu.com"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("URLRequestDemo"));
    if (!ok) {
        LogLine(L"FBroHsCreate failed.");
    }
}

class InitEvent final : public FBroHsInitEvent {
public:
    void OnContextInitialized() override {
        g_fbro_ready = true;
        LogLine(L"FBro context initialized.");
        CreateEmbeddedBaiduBrowser();
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
};

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_global_button = CreateWindowExW(0, L"BUTTON",
            L"\u521b\u5efa\u5168\u5c40URL\u8bf7\u6c42",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 16, 180, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonGlobalRequest)),
            GetModuleHandleW(nullptr), nullptr);
        g_frame_button = CreateWindowExW(0, L"BUTTON",
            L"\u521b\u5efa\u6846\u67b6URL\u8bf7\u6c42",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            208, 16, 180, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonFrameRequest)),
            GetModuleHandleW(nullptr), nullptr);
        g_status_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kButtonGlobalRequest:
            CreateGlobalURLRequest();
            return 0;
        case kButtonFrameRequest:
            CreateFrameURLRequest();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case kMsgAppendLog:
        if (wparam == 1) {
            RemoveRequestClient(reinterpret_cast<CefURLRequestClient*>(lparam));
            return 0;
        }
        if (auto* log = reinterpret_cast<LogMessage*>(lparam)) {
            AppendStatusDirect(log->text);
            delete log;
        }
        return 0;
    case WM_TIMER:
        if (wparam == kCloseFallbackTimer && g_close_requested) {
            LogLine(L"Close fallback timer fired.");
            g_browser = nullptr;
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
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    g_main_window = CreateWindowExW(WS_EX_APPWINDOW, kMainWindowClass,
        L"\u521b\u5efaURL\u8bf7\u6c42",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1240, 820,
        nullptr, nullptr, instance, nullptr);
    if (!g_main_window) {
        return false;
    }

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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"URLRequestDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"URLRequestDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"URLRequestDemo", MB_ICONERROR);
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
