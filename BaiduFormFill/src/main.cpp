#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"

#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kMainWindowClass[] = L"BaiduFormFill.MainWindow";
constexpr int kButtonFill = 1001;
constexpr int kToolbarHeight = 56;

HWND g_main_window = nullptr;
HWND g_fill_button = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
bool g_fbro_ready = false;

std::filesystem::path ExeDir() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

std::string ToSystemAnsi(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());

    std::ofstream out(ExeDir() / L"baidu-form-fill.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

void LayoutChildren() {
    if (!g_main_window) {
        return;
    }

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    if (g_fill_button) {
        MoveWindow(g_fill_button, 16, 12, 140, 32, TRUE);
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

void ExecuteBaiduFill() {
    if (!g_browser) {
        MessageBoxW(g_main_window, L"Browser is not ready.", L"BaiduFormFill", MB_ICONINFORMATION);
        return;
    }

    const int browser_id = FBroHsBrowser_GetIdentifier(g_browser);
    const BOOL has_document = FBroHsBrowser_HasDocument(g_browser);
    LogLine(L"Baidu fill clicked. browser_id=" + std::to_wstring(browser_id) +
        L", has_document=" + std::to_wstring(has_document));

    CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(g_browser);
    if (!frame) {
        LogLine(L"Baidu fill failed: FBro main frame is null.");
        MessageBoxW(g_main_window, L"Main frame is not ready.", L"BaiduFormFill", MB_ICONINFORMATION);
        return;
    }

    static const wchar_t script[] = LR"JS(
(function () {
  var input = document.querySelector("#kw") ||
              document.querySelector("input[id*='kw']") ||
              document.querySelector("input[name='wd']") ||
              document.querySelector("textarea[name='wd']");
  if (!input) {
    console.warn("[BaiduFormFill] input not found");
    return;
  }

  var value = "FBrowserCEF3lib\u6846\u67b6";
  input.focus();
  input.click();

  var proto = Object.getPrototypeOf(input);
  var descriptor = Object.getOwnPropertyDescriptor(proto, "value");
  if (descriptor && descriptor.set) {
    descriptor.set.call(input, value);
  } else {
    input.value = value;
  }

  input.dispatchEvent(new Event("input", { bubbles: true }));
  input.dispatchEvent(new Event("change", { bubbles: true }));
  input.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key: "Enter", code: "Enter", keyCode: 13, which: 13 }));

  var button = document.querySelector("#su") ||
               document.querySelector("input[type='submit']") ||
               document.querySelector("button[type='submit']");
  if (button) {
    setTimeout(function () { button.click(); }, 100);
  }
})();
)JS";

    FBroHsBrowserFrame_ExecuteJavaScript(frame, CefString(script),
        CefString(L"baidu-form-fill"), 1);
    LogLine(L"Baidu fill JavaScript executed.");
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
    extra->SetString("flag", "BaiduFormFill");

    const BOOL ok = FBroHsCreate(CefString("https://www.baidu.com"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("BaiduFormFill"));
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
        g_fill_button = CreateWindowExW(0, L"BUTTON",
            L"\u767e\u5ea6\u586b\u8868",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 12, 140, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonFill)),
            GetModuleHandleW(nullptr), nullptr);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kButtonFill) {
            ExecuteBaiduFill();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case WM_CLOSE:
        if (g_browser) {
            FBroHsBrowserHost_CloseBrowser(g_browser, true);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        FBroQuitMessageLoop();
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
        L"\u767e\u5ea6\u586b\u8868",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"BaiduFormFill", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"BaiduFormFill", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"BaiduFormFill", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    FBroRunMessageLoop();
    FBroShutdown(TRUE);
    WSACleanup();
    if (SUCCEEDED(co_result)) CoUninitialize();
    return 0;
}
