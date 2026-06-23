#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroCookieManager.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
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

constexpr wchar_t kMainWindowClass[] = L"CookieSettingsDemo.MainWindow";
constexpr int kToolbarHeight = 196;
constexpr int kButtonCookieUrl = 1001;
constexpr int kButtonCookieGlobalGet = 1002;
constexpr int kButtonCookieGlobalSet = 1003;
constexpr int kButtonCookieBrowserGet = 1004;
constexpr int kButtonCookieBrowserSet = 1005;
constexpr int kButtonCookieDelete = 1006;

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
std::vector<HWND> g_buttons;
std::vector<CefRefPtr<FBroHsCookieVisitor>> g_cookie_visitors;
bool g_fbro_ready = false;

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

std::wstring FromUtf8(const char* value) {
    if (!value || value[0] == '\0') return {};
    const int len = static_cast<int>(strlen(value));
    const int size = MultiByteToWideChar(CP_UTF8, 0, value, len, nullptr, 0);
    if (size <= 0) {
        const int ansi_size = MultiByteToWideChar(CP_ACP, 0, value, len, nullptr, 0);
        std::wstring ansi_result(ansi_size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, value, len, ansi_result.data(), ansi_size);
        return ansi_result;
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, len, result.data(), size);
    return result;
}

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"cookie-settings-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

void Notify(const std::wstring& text) {
    LogLine(text);
    MessageBoxW(g_main_window, text.c_str(),
        L"Cookie\u8bbe\u7f6e\u53d6\u51fa", MB_ICONINFORMATION);
}

std::wstring CookieToText(POINT_COOKIEDATA cookie) {
    if (!cookie) return L"<null cookie>";
    std::wstringstream stream;
    stream << FromUtf8(cookie->name) << L"=" << FromUtf8(cookie->value)
           << L"; domain=" << FromUtf8(cookie->domain)
           << L"; path=" << FromUtf8(cookie->path)
           << L"; httpOnly=" << cookie->httponly
           << L"; secure=" << cookie->secure;
    return stream.str();
}

class CookieVisitor final : public FBroHsCookieVisitor {
public:
    explicit CookieVisitor(std::wstring title) : title_(std::move(title)) {}

    void Start() override {
        items_.clear();
        LogLine(title_ + L": visit start.");
    }

    bool Visit(POINT_COOKIEDATA cookie, int count, int total, bool&) override {
        std::wstringstream stream;
        stream << L"[" << (count + 1) << L"/" << total << L"] "
               << CookieToText(cookie);
        items_.push_back(stream.str());
        LogLine(title_ + L": " + stream.str());
        return true;
    }

    void End() override {
        std::wstringstream stream;
        stream << title_ << L": visit end, count=" << items_.size();
        if (!items_.empty()) {
            stream << L"\n\n";
            const size_t max_items = std::min<size_t>(items_.size(), 12);
            for (size_t i = 0; i < max_items; ++i) {
                stream << items_[i] << L"\n";
            }
            if (items_.size() > max_items) {
                stream << L"...";
            }
        }
        Notify(stream.str());
    }

private:
    std::wstring title_;
    std::vector<std::wstring> items_;

    IMPLEMENT_REFCOUNTING(CookieVisitor);
};

CefRefPtr<CefCookieManager> GlobalCookieManager() {
    return FBroHsCookieManager_GetGlobalManager();
}

CefRefPtr<CefCookieManager> BrowserCookieManager() {
    if (!g_browser) return nullptr;
    auto context = FBroHsBrowserHost_GetRequestContext(g_browser);
    if (!context) return nullptr;
    return FBroHsRequestContext_GetCookieManager(context);
}

void KeepVisitor(CefRefPtr<CookieVisitor> visitor) {
    g_cookie_visitors.push_back(visitor);
}

void VisitUrlCookie() {
    auto manager = GlobalCookieManager();
    CefRefPtr<CookieVisitor> visitor = new CookieVisitor(L"\u53d6Cookie_url");
    KeepVisitor(visitor);
    const BOOL ok = FBroHsCookieManager_VisitUrlCookies(
        manager, CefString("http://www.baidu.com/"), TRUE, visitor);
    LogLine(L"\u53d6Cookie_url submitted=" + std::to_wstring(ok));
}

void VisitGlobalCookie() {
    auto manager = GlobalCookieManager();
    CefRefPtr<CookieVisitor> visitor = new CookieVisitor(L"\u53d6Cookie_\u5168\u5c40");
    KeepVisitor(visitor);
    const BOOL ok = FBroHsCookieManager_VisitAllCookies(manager, visitor);
    LogLine(L"\u53d6Cookie_\u5168\u5c40 submitted=" + std::to_wstring(ok));
}

void SetCookieOn(CefRefPtr<CefCookieManager> manager, const std::wstring& title) {
    if (!manager) {
        Notify(title + L": CookieManager is null.");
        return;
    }

    std::string name = "fbro_demo_cookie";
    std::string value = "global-or-browser-cookie";
    std::string domain = ".baidu.com";
    std::string path = "/";

    E_COOKIEDATA data{};
    data.name = name.data();
    data.value = value.data();
    data.domain = domain.data();
    data.path = path.data();
    data.httponly = FALSE;
    data.has_expires = FALSE;
    data.secure = 0;
    data.same_site = 0;
    data.priority = 0;

    const BOOL ok = FBroHsCookieManager_SetCookie(
        manager, CefString("http://www.baidu.com"), &data);
    FBroHsCookieManager_FlushStore(manager);
    Notify(title + L": SetCookie result=" + std::to_wstring(ok) +
        L"\nname=fbro_demo_cookie\nvalue=global-or-browser-cookie");
}

void VisitBrowserCookie() {
    auto manager = BrowserCookieManager();
    if (!manager) {
        Notify(L"\u53d6Cookie: browser cookie manager is null.");
        return;
    }
    CefRefPtr<CookieVisitor> visitor = new CookieVisitor(L"\u53d6Cookie");
    KeepVisitor(visitor);
    const BOOL ok = FBroHsCookieManager_VisitAllCookies(manager, visitor);
    LogLine(L"\u53d6Cookie submitted=" + std::to_wstring(ok));
}

void DeleteCookie() {
    auto global_manager = GlobalCookieManager();
    auto browser_manager = BrowserCookieManager();
    const BOOL ok_global = FBroHsCookieManager_DeleteCookies(
        global_manager, CefString(""), CefString(""));
    BOOL ok_browser = FALSE;
    if (browser_manager) {
        ok_browser = FBroHsCookieManager_DeleteCookies(
            browser_manager, CefString(""), CefString(""));
        FBroHsCookieManager_FlushStore(browser_manager);
    }
    FBroHsCookieManager_FlushStore(global_manager);
    Notify(L"\u5220\u9664Cookie: global=" + std::to_wstring(ok_global) +
        L", browser=" + std::to_wstring(ok_browser));
}

void HandleAction(int id) {
    switch (id) {
    case kButtonCookieUrl:
        VisitUrlCookie();
        break;
    case kButtonCookieGlobalGet:
        VisitGlobalCookie();
        break;
    case kButtonCookieGlobalSet:
        SetCookieOn(GlobalCookieManager(), L"\u7f6eCookie_\u5168\u5c40");
        break;
    case kButtonCookieBrowserGet:
        VisitBrowserCookie();
        break;
    case kButtonCookieBrowserSet:
        SetCookieOn(BrowserCookieManager(), L"\u7f6eCookie");
        break;
    case kButtonCookieDelete:
        DeleteCookie();
        break;
    }
}

void LayoutChildren() {
    if (!g_main_window) return;
    RECT rc{};
    GetClientRect(g_main_window, &rc);

    const int button_width = 180;
    const int button_height = 28;
    const int x = 16;
    int y = 12;
    for (HWND button : g_buttons) {
        MoveWindow(button, x, y, button_width, button_height, TRUE);
        y += 30;
    }

    if (g_browser_host) {
        MoveWindow(g_browser_host, 220, 0,
            max(1, rc.right - 220),
            max(1, rc.bottom - rc.top),
            TRUE);
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
    extra->SetString("flag", "CookieSettingsDemo");

    const BOOL ok = FBroHsCreate(CefString("https://www.baidu.com"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("CookieSettingsDemo"));
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

HWND CreateButton(HWND parent, const wchar_t* text, int id) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    g_buttons.push_back(button);
    return button;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        CreateButton(hwnd, L"\u53d6Cookie_url", kButtonCookieUrl);
        CreateButton(hwnd, L"\u53d6Cookie_\u5168\u5c40", kButtonCookieGlobalGet);
        CreateButton(hwnd, L"\u7f6eCookie_\u5168\u5c40", kButtonCookieGlobalSet);
        CreateButton(hwnd, L"\u53d6Cookie", kButtonCookieBrowserGet);
        CreateButton(hwnd, L"\u7f6eCookie", kButtonCookieBrowserSet);
        CreateButton(hwnd, L"\u5220\u9664Cookie", kButtonCookieDelete);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        HandleAction(LOWORD(wparam));
        return 0;
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
    if (!RegisterClassExW(&wc)) return false;

    g_main_window = CreateWindowExW(WS_EX_APPWINDOW, kMainWindowClass,
        L"Cookie\u8bbe\u7f6e\u53d6\u51fa",
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"CookieSettingsDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"CookieSettingsDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"CookieSettingsDemo", MB_ICONERROR);
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
