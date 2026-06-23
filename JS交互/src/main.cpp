#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroString.h"

#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kMainWindowClass[] = L"JSInteractionDemo.MainWindow";
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
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

std::wstring FromFBroString(CefRefPtr<FBroString> value) {
    if (!value) return {};
    const int size = FBroString_WSize(value);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    FBroString_GetWcharData(value, result.data());
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"js-interaction-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

std::wstring JsStringLiteral(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': result += L"\\\\"; break;
        case L'"': result += L"\\\""; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default: result += ch; break;
        }
    }
    result += L"\"";
    return result;
}

int MaxInt(int a, int b) {
    return a > b ? a : b;
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

    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, 0, width, height, TRUE);
    }
    if (g_browser && g_browser_host) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd) {
            RECT host_rc{};
            GetClientRect(g_browser_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                MaxInt(1, host_rc.right - host_rc.left),
                MaxInt(1, host_rc.bottom - host_rc.top),
                TRUE);
        }
    }
}

std::wstring HtmlPage() {
    return LR"HTML(<!doctype html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Test Cef Query</title>
  <style>
    body { font-family: "Segoe UI", "Microsoft YaHei", sans-serif; padding: 28px; color: #1b1f24; }
    h1 { font-size: 24px; margin: 0 0 18px; }
    .row { margin: 14px 0; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    input[type="text"] { width: 320px; padding: 8px 10px; font-size: 15px; }
    input[type="button"] { padding: 8px 14px; font-size: 15px; cursor: pointer; }
    #log { margin-top: 20px; padding: 14px; min-height: 160px; background: #f5f7fb; border: 1px solid #d8dee9; white-space: pre-wrap; }
  </style>
  <script>
    var request_id = 0;
    var next_id = 1;

    function log(text) {
      var box = document.getElementById("log");
      box.textContent += text + "\n";
    }

    function nativeQuery(functionName, value) {
      request_id = next_id++;
      log("JS -> C++ [" + functionName + "] id=" + request_id + " request=" + value);
      console.log("__FBRO_JS_QUERY__" + JSON.stringify({
        fn: functionName,
        id: request_id,
        request: value,
        persistent: true
      }));
    }

    function doQuery() {
      nativeQuery("cefQuery", document.getElementById("query").value);
    }

    function doQuery1() {
      nativeQuery("cefQuerytest", document.getElementById("query1").value);
    }

    function doQueryCancel() {
      log("JS -> C++ cancel id=" + request_id);
      console.log("__FBRO_JS_QUERY_CANCEL__" + JSON.stringify({ id: request_id }));
    }

    window.nativeQuerySuccess = function(id, response) {
      log("C++ -> JS success id=" + id + " response=" + response);
      alert(response);
    };

    window.nativeQueryCanceled = function(id) {
      log("C++ -> JS canceled id=" + id);
    };
  </script>
</head>
<body>
  <h1>JS&#20132;&#20114;&#27979;&#35797;</h1>
  <form onsubmit="return false;">
    <div class="row">
      <label>Query String1:</label>
      <input type="text" id="query" value="&#20320;&#22909;CefQuery">
      <input type="button" value="SendQuery" onclick="doQuery()">
      <input type="button" value="Cancel" onclick="doQueryCancel()">
    </div>
    <div class="row">
      <label>Query String2:</label>
      <input type="text" id="query1" value="GiveMeMoney">
      <input type="button" value="SendQuery1" onclick="doQuery1()">
    </div>
  </form>
  <div id="log"></div>
</body>
</html>)HTML";
}

std::wstring DataUriForHtml() {
    return FromFBroString(FBroHsGetDataURI(CefString(L"text/html"),
        CefString(HtmlPage())));
}

std::wstring ExtractJsonString(const std::wstring& json, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\":\"";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) return {};
    pos += marker.size();
    std::wstring result;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        wchar_t ch = json[pos];
        if (escaped) {
            switch (ch) {
            case L'n': result += L'\n'; break;
            case L'r': result += L'\r'; break;
            case L't': result += L'\t'; break;
            default: result += ch; break;
            }
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        if (ch == L'"') break;
        result += ch;
    }
    return result;
}

int64_t ExtractJsonInt(const std::wstring& json, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\":";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) return 0;
    pos += marker.size();
    while (pos < json.size() && iswspace(json[pos])) ++pos;
    int64_t value = 0;
    while (pos < json.size() && iswdigit(json[pos])) {
        value = value * 10 + (json[pos] - L'0');
        ++pos;
    }
    return value;
}

void SendJsSuccess(CefRefPtr<CefBrowser> browser, int64_t id,
                   const std::wstring& response) {
    if (!browser) return;
    CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(browser);
    if (!frame) return;
    const std::wstring script = L"window.nativeQuerySuccess(" +
        std::to_wstring(id) + L"," + JsStringLiteral(response) + L");";
    FBroHsBrowserFrame_ExecuteJavaScript(frame, CefString(script),
        CefString(L"native-js-interaction"), 1);
}

void SendJsCanceled(CefRefPtr<CefBrowser> browser, int64_t id) {
    if (!browser) return;
    CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(browser);
    if (!frame) return;
    const std::wstring script = L"window.nativeQueryCanceled(" +
        std::to_wstring(id) + L");";
    FBroHsBrowserFrame_ExecuteJavaScript(frame, CefString(script),
        CefString(L"native-js-interaction"), 1);
}

class BrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        LogLine(L"JS interaction browser created.");
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

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t,
                          const CefString& message,
                          const CefString&,
                          int) override {
        const std::wstring text = message.ToWString();
        const std::wstring query_prefix = L"__FBRO_JS_QUERY__";
        const std::wstring cancel_prefix = L"__FBRO_JS_QUERY_CANCEL__";

        if (text.rfind(query_prefix, 0) == 0) {
            const std::wstring json = text.substr(query_prefix.size());
            const int64_t id = ExtractJsonInt(json, L"id");
            const std::wstring fn = ExtractJsonString(json, L"fn");
            const std::wstring request = ExtractJsonString(json, L"request");
            LogLine(L"OnQuery fn=" + fn + L", id=" + std::to_wstring(id) +
                L", request=" + request);
            SendJsSuccess(browser, id, request + L":success");
            CreatePopupBrowser();
            return true;
        }

        if (text.rfind(cancel_prefix, 0) == 0) {
            const std::wstring json = text.substr(cancel_prefix.size());
            const int64_t id = ExtractJsonInt(json, L"id");
            LogLine(L"OnQueryCanceled id=" + std::to_wstring(id));
            SendJsCanceled(browser, id);
            return true;
        }

        return false;
    }

private:
    void CreatePopupBrowser() {
        E_WINDOWS_INFO window_info{};
        window_info.is_null = FALSE;
        window_info.parent_window = nullptr;
        window_info.x = CW_USEDEFAULT;
        window_info.y = CW_USEDEFAULT;
        window_info.width = 1024;
        window_info.height = 800;
        window_info.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        window_info.ex_style = WS_EX_APPWINDOW;
        window_info.windowless_rendering_enabled = FALSE;
        window_info.shared_texture_enabled = FALSE;
        window_info.external_begin_frame_enabled = FALSE;

        FBroBrowserSetting browser_setting{};
        browser_setting.is_null = TRUE;
        browser_setting.background_color = 0x00FFFFFF;

        CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
        extra->SetString("JSInteraction", "CreatedByQuery");

        CefRefPtr<BrowserEvent> event = new BrowserEvent();
        FBroHsCreate(CefString("https://www.baidu.com/"),
            &window_info, &browser_setting, nullptr, extra, event, nullptr,
            CefString("JSInteractionPopup"));
    }

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
    extra->SetString("flag", "JSInteractionMain");

    const std::wstring uri = DataUriForHtml();
    const BOOL ok = FBroHsCreate(CefString(uri),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("JSInteractionMain"));
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
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_SIZE:
        LayoutChildren();
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
    if (!RegisterClassExW(&wc)) return false;

    g_main_window = CreateWindowExW(WS_EX_APPWINDOW, kMainWindowClass,
        L"JS\u4ea4\u4e92",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780,
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"JSInteractionDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"JSInteractionDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"JSInteractionDemo", MB_ICONERROR);
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
