#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"

#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ResourceTamperDemo.MainWindow";
constexpr int kToolbarHeight = 96;
constexpr UINT kMsgMaybeDestroy = WM_APP + 401;
constexpr UINT kMsgAppendLog = WM_APP + 402;
constexpr UINT kMsgLoadBaidu = WM_APP + 403;
constexpr UINT_PTR kCloseFallbackTimer = 501;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_status_box = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefRegistration> g_devtools_registration;
bool g_fbro_ready = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;

struct LogMessage {
    std::wstring text;
};

template <typename T>
T MaxValue(T a, T b) {
    return a > b ? a : b;
}

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
    std::ofstream out(ExeDir() / L"resource-tamper-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

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

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[7]{};
                sprintf_s(buf, "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::wstring HtmlText() {
    return LR"HTML(<!doctype html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Test Cef</title>
  <style>
    body { font-family: "Microsoft YaHei", sans-serif; background: white; padding: 40px; }
    h2 { color: #222; }
    .green { color: #00aa00; font-size: 18px; }
    .red { color: red; margin-top: 12px; }
    a { display: inline-block; margin-top: 18px; color: #1a73e8; }
  </style>
</head>
<body>
  <h2>&#20320;&#22909; FBrowser&#65281;&#65281;&#65281;</h2>
  <div class="green">&#36825;&#26159;&#19968;&#20010;&#30452;&#25509;&#23558;&#30334;&#24230;&#32593;&#39029;&#26367;&#25442;&#25481;&#30340;&#20363;&#23376;&#65281;</div>
  <div class="red">&#36825;&#37324;&#26159;&#20010; div</div>
  <a href="https://www.baidu.com">&#25171;&#24320;&#30334;&#24230;&#32593;&#39029;&#65288;&#20250;&#32487;&#32493;&#34987;&#26412;&#31034;&#20363;&#31613;&#25913;&#26367;&#25442;&#65289;</a>
  <script>console.log("Hello FBrowser");</script>
</body>
</html>)HTML";
}

void DevToolsContinueRequest(CefRefPtr<CefBrowser> browser,
                             const std::wstring& request_id) {
    if (!browser) return;
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("requestId", request_id);
    browser->GetHost()->ExecuteDevToolsMethod(
        2001, CefString("Fetch.continueRequest"), params);
}

void DevToolsFulfillRequest(CefRefPtr<CefBrowser> browser,
                            const std::wstring& request_id) {
    if (!browser) return;

    const std::string html = ToUtf8(HtmlText());
    const CefString encoded = CefBase64Encode(html.data(), html.size());

    std::ostringstream message;
    message << "{\"id\":2002,\"method\":\"Fetch.fulfillRequest\",\"params\":{"
            << "\"requestId\":\"" << JsonEscape(ToUtf8(request_id)) << "\","
            << "\"responseCode\":200,"
            << "\"responseHeaders\":["
            << "{\"name\":\"Content-Type\",\"value\":\"text/html; charset=utf-8\"},"
            << "{\"name\":\"Cache-Control\",\"value\":\"no-store\"}"
            << "],"
            << "\"body\":\"" << JsonEscape(encoded.ToString()) << "\""
            << "}}";

    const std::string json = message.str();
    browser->GetHost()->SendDevToolsMessage(json.data(), json.size());
    PostStatus(L"tampered baidu main document, requestId=" + request_id);
}

bool IsTargetBaiduDocument(const std::wstring& url,
                           const std::wstring& resource_type) {
    return resource_type == L"Document" &&
        (url == L"https://www.baidu.com/" ||
         url == L"https://www.baidu.com");
}

class DevToolsObserver final : public CefDevToolsMessageObserver {
public:
    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser,
                         const CefString& method,
                         const void* params,
                         size_t params_size) override {
        if (!browser || !params || params_size == 0) {
            return;
        }

        if (FromCefString(method) != L"Fetch.requestPaused") {
            return;
        }

        const std::string json(static_cast<const char*>(params), params_size);
        CefRefPtr<CefValue> parsed = CefParseJSON(json, JSON_PARSER_RFC);
        if (!parsed || !parsed->IsValid() || parsed->GetType() != VTYPE_DICTIONARY) {
            return;
        }

        CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
        const std::wstring request_id = FromCefString(dict->GetString("requestId"));
        const std::wstring resource_type = FromCefString(dict->GetString("resourceType"));
        CefRefPtr<CefDictionaryValue> request = dict->GetDictionary("request");
        const std::wstring url = request ? FromCefString(request->GetString("url")) : L"";

        if (request_id.empty()) {
            return;
        }

        if (IsTargetBaiduDocument(url, resource_type)) {
            DevToolsFulfillRequest(browser, request_id);
            return;
        }

        DevToolsContinueRequest(browser, request_id);
    }

private:
    IMPLEMENT_REFCOUNTING(DevToolsObserver);
};

void AttachFetchTamperObserver(CefRefPtr<CefBrowser> browser) {
    if (!browser || g_devtools_registration) {
        return;
    }

    CefRefPtr<DevToolsObserver> observer = new DevToolsObserver();
    g_devtools_registration =
        browser->GetHost()->AddDevToolsMessageObserver(observer);

    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    CefRefPtr<CefListValue> patterns = CefListValue::Create();
    CefRefPtr<CefDictionaryValue> pattern = CefDictionaryValue::Create();
    pattern->SetString("urlPattern", "*");
    pattern->SetString("requestStage", "Request");
    patterns->SetDictionary(0, pattern);
    params->SetList("patterns", patterns);
    browser->GetHost()->ExecuteDevToolsMethod(
        2000, CefString("Fetch.enable"), params);
    PostStatus(L"DevTools Fetch tamper observer attached.");
}

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    if (g_status_box) {
        MoveWindow(g_status_box, 12, 10, MaxValue(1, width - 24), 76, TRUE);
    }
    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, kToolbarHeight, width,
            MaxValue(1, height - kToolbarHeight), TRUE);
    }
    if (g_browser) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd && g_browser_host) {
            RECT host_rc{};
            GetClientRect(g_browser_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                MaxValue(1L, host_rc.right - host_rc.left),
                MaxValue(1L, host_rc.bottom - host_rc.top),
                TRUE);
        }
    }
}

void LoadBaiduAfterObserverAttached() {
    if (!g_browser) return;
    CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(g_browser);
    if (!frame) return;
    FBroHsBrowserFrame_LoadURL(frame, CefString("https://www.baidu.com"));
    PostStatus(L"loaded https://www.baidu.com after Fetch observer attached.");
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

class BrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        PostStatus(L"browser created.");
        LayoutChildren();
        AttachFetchTamperObserver(browser);
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgLoadBaidu, 0, 0);
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser == g_browser) {
            g_browser = nullptr;
            g_devtools_registration = nullptr;
        }
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgMaybeDestroy, 0, 0);
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>,
                   CefRefPtr<CefFrame> frame,
                   int http_status_code) override {
        if (frame && frame->IsMain()) {
            PostStatus(L"main page load end, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

void CreateEmbeddedBrowser() {
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
    window_info.width = MaxValue<LONG>(1, rc.right - rc.left);
    window_info.height = MaxValue<LONG>(1, rc.bottom - rc.top);
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
    extra->SetString("flag", "ResourceTamperDemo");

    const BOOL ok = FBroHsCreate(CefString("about:blank"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("ResourceTamperDemo"));
    if (!ok) {
        PostStatus(L"FBroHsCreate failed.");
    }
}

class InitEvent final : public FBroHsInitEvent {
public:
    void OnContextInitialized() override {
        g_fbro_ready = true;
        PostStatus(L"FBro context initialized.");
        CreateEmbeddedBrowser();
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
};

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_status_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
            12, 10, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        AppendStatusDirect(L"\u81ea\u52a8\u7be1\u6539\u767e\u5ea6\u9996\u9875\u8bf7\u6c42\uff1a\u76ee\u6807 URL \u4f1a\u88ab\u66ff\u6362\u4e3a\u672c\u5730 HTML\u3002");
        LayoutChildren();
        return 0;
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case WM_TIMER:
        if (wparam == kCloseFallbackTimer && g_close_requested) {
            LogLine(L"Close fallback timer fired.");
            g_browser = nullptr;
            g_devtools_registration = nullptr;
            MaybeDestroyAfterClose();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case kMsgMaybeDestroy:
        MaybeDestroyAfterClose();
        return 0;
    case kMsgAppendLog: {
        auto* message = reinterpret_cast<LogMessage*>(lparam);
        if (message) {
            AppendStatusDirect(message->text);
            delete message;
        }
        return 0;
    }
    case kMsgLoadBaidu:
        LoadBaiduAfterObserverAttached();
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
        L"\u7be1\u6539\u8d44\u6e90\u5b9e\u4f8b",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"ResourceTamperDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"ResourceTamperDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"ResourceTamperDemo", MB_ICONERROR);
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
