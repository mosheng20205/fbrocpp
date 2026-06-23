#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroString.h"
#include "FBroVIPEventInterface.h"
#include "FBroVIPStruct.h"
#include "FBroVIPUserAgentData.h"
#include "FBroVIPControl.h"
#include "FBroVIPInterface.h"

#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"NativeFBroDemo.MainWindow";
constexpr int kButtonCreateEmbedded = 1001;
constexpr int kButtonCreateNative = 1002;
constexpr int kToolbarHeight = 64;
HWND g_main_window = nullptr;
HWND g_embed_button = nullptr;
HWND g_native_button = nullptr;
HWND g_embed_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefBrowser> g_native_browser;
std::vector<CefRefPtr<CefClient>> g_native_clients;
bool g_fbro_context_ready = false;

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

std::filesystem::path ExeDir() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
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

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring FromFBroString(CefRefPtr<FBroString> value) {
    if (!value) {
        return L"<null>";
    }

    const int size = value->WSize();
    if (size <= 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(size) + 1, L'\0');
    value->GetWcharData(buffer.data());
    return std::wstring(buffer.data());
}

std::string Trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
           value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }

    return value.substr(start);
}

std::filesystem::path FindEnvFile() {
    auto dir = ExeDir();
    for (int i = 0; i < 6; ++i) {
        const auto candidate = dir / L".env";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }

        if (!dir.has_parent_path() || dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }

    return {};
}

std::string ReadEnvValue(const std::string& key) {
    const auto env_file = FindEnvFile();
    if (env_file.empty()) {
        return {};
    }

    std::ifstream in(env_file, std::ios::binary);
    std::string line;
    const auto prefix = key + "=";
    while (std::getline(in, line)) {
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        line = Trim(line);
        if (line.rfind(prefix, 0) == 0) {
            return Trim(line.substr(prefix.size()));
        }
    }

    return {};
}

void LogLine(const std::wstring& message);

bool ApplyVipLicenseFromEnv() {
    const auto key = ReadEnvValue("FBRO_VIP_LICENSE_KEY");
    if (key.empty()) {
        LogLine(L"No FBRO_VIP_LICENSE_KEY found in .env.");
        return false;
    }

    const auto process_type = FBroGetProcessType();
    if (process_type != BrowserProcess) {
        LogLine(L"Skip VIP license setup: current process is not BrowserProcess.");
        return true;
    }

    LogLine(L"VIP license key loaded from .env, calling FBroHsOnlineLicenseControl_SetKey.");
    struct SetKeyState {
        std::atomic_bool done{false};
        BOOL result{FALSE};
    };

    auto state = std::make_shared<SetKeyState>();
    std::thread([key, state]() {
        const CefString license_key(FromUtf8(key));
        state->result = FBroHsOnlineLicenseControl_SetKey(license_key);
        state->done = true;
    }).detach();

    for (int i = 0; i < 300; ++i) {
        if (state->done) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!state->done) {
        LogLine(L"FBroHsOnlineLicenseControl_SetKey did not return within 30 seconds.");
        return false;
    }

    std::wstringstream stream;
    stream << L"VIP license key loaded from .env, length=" << key.size()
           << L", SetKeyResult=" << state->result;
    LogLine(stream.str());
    return state->result == TRUE;
}

void LogLine(const std::wstring& message) {
    const auto line = L"[VIP TEST] " + message + L"\n";
    OutputDebugStringW(line.c_str());

    const auto log_file = ExeDir() / L"vip-test.log";
    std::ofstream out(log_file, std::ios::app | std::ios::binary);
    out << ToUtf8(line);
}

class VipRuntimeResultCallback final : public FBroHsGeneralResultCallback {
public:
    void Callback_Data(CefRefPtr<CefBrowser>,
                       int message_id,
                       bool success,
                       const void* data,
                       size_t data_size) override {
        std::wstringstream stream;
        stream << L"RuntimeEvaluate callback: message_id=" << message_id
               << L", success=" << (success ? L"true" : L"false")
               << L", bytes=" << data_size;
        if (data && data_size > 0) {
            const auto text = std::string(
                static_cast<const char*>(data),
                static_cast<const char*>(data) + data_size);
            stream << L", json=" << FromUtf8(text);
        }
        LogLine(stream.str());
    }

private:
    IMPLEMENT_REFCOUNTING(VipRuntimeResultCallback);
};

void LogVipLibraryInfo() {
    LogLine(L"FBrowserVIP.dll availability test started.");
    LogLine(L"FBroBrowser_IsLicenceKey=" +
        std::to_wstring(FBroBrowser_IsLicenceKey()));
    LogLine(L"MachineCode=" + FromFBroString(FBroHsBrowser_GetMachineCode()));
    LogLine(L"LicenseStartDate=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseStartDate()));
    LogLine(L"LicenseEndDate=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseEndDate()));
    LogLine(L"LicenseFunction=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseFunction()));
    LogLine(L"LicenseVersion=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseDevTool()));
    LogLine(L"LicenseType=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseType()));
    LogLine(L"LicenseTargetPlatform=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetShowLicenseSysVersion()));
    LogLine(L"LicenseError=" +
        FromFBroString(FBroHsOnlineLicenseControl_GetError()));
}

void TestVipControl(CefRefPtr<CefBrowser> browser) {
    if (!browser) {
        LogLine(L"VIPControl test skipped: browser is null.");
        return;
    }

    auto vip_control = FBroHsBrowser_GetVIPControl(browser);
    const BOOL is_null = FBroHsVIPControl_IsNULL(vip_control);
    LogLine(L"FBroHsBrowser_GetVIPControl IsNULL=" + std::to_wstring(is_null));
    if (is_null) {
        return;
    }

    FBroHsVIPControl_ClearAllData(vip_control);
    FBroHsVIPControl_ClearFingerCount(vip_control);
    FBroHsVIPControl_SetVirWebdriver(vip_control, FALSE);
    FBroHsVIPControl_SetVirPlatform(vip_control, CefString("Win32"));
    FBroHsVIPControl_SetVirVendor(vip_control, CefString("Google Inc."));
    FBroHsVIPControl_SetVirHardwareConcurrency(vip_control, 8);
    FBroHsVIPControl_SetVirDeviceMemory(vip_control, 8);
    FBroHsVIPControl_SetVirLanguages(vip_control, CefString("zh-CN,zh,en-US,en"));
    FBroHsVIPControl_SetVirAcceptlanguages(vip_control, CefString("zh-CN,zh;q=0.9,en;q=0.8"));
    FBroHsVIPControl_SetTouchEventEmulationEnabled(vip_control, TRUE, 5);

    LogLine(L"VIP fingerprint setters called successfully.");
    LogLine(L"FingerCount=" +
        FromFBroString(FBroHsVIPControl_GetFingerCount(vip_control)));

    CefRefPtr<VipRuntimeResultCallback> callback = new VipRuntimeResultCallback();
    FBroHsVIPControl_RuntimeEnable(vip_control, TRUE);
    FBroHsVIPControl_RuntimeEvaluate(
        vip_control,
        CefString("({vipApiAvailable:true, webdriver:navigator.webdriver, platform:navigator.platform, languages:navigator.languages})"),
        TRUE,
        0,
        FALSE,
        FALSE,
        5000,
        FALSE,
        FALSE,
        callback,
        nullptr,
        1001);
    LogLine(L"RuntimeEvaluate request submitted.");
}

void LayoutBrowser() {
    if (!g_main_window) {
        return;
    }

    RECT rc{};
    GetClientRect(g_main_window, &rc);

    if (g_embed_button) {
        MoveWindow(g_embed_button, 16, 16, 260, 32, TRUE);
    }
    if (g_native_button) {
        MoveWindow(g_native_button, 292, 16, 220, 32, TRUE);
    }
    if (g_embed_host) {
        MoveWindow(g_embed_host, 0, kToolbarHeight,
            rc.right - rc.left,
            max(1, rc.bottom - rc.top - kToolbarHeight),
            TRUE);
    }

    if (g_browser) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd && g_embed_host) {
            RECT host_rc{};
            GetClientRect(g_embed_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                host_rc.right - host_rc.left,
                host_rc.bottom - host_rc.top,
                TRUE);
        }
    }
}

class DemoBrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        LogLine(L"Browser created by FBroHsCreate.");
        TestVipControl(g_browser);
        LayoutBrowser();
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
            LogLine(L"Browser main frame load ended, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(DemoBrowserEvent);
};

class DemoInitEvent final : public FBroHsInitEvent {
public:
    void OnContextInitialized() override {
        g_fbro_context_ready = true;
        LogVipLibraryInfo();
        LogLine(L"FBro context initialized. Click a button to create a browser.");
    }

private:
    IMPLEMENT_REFCOUNTING(DemoInitEvent);
};

void CreateEmbeddedBrowser() {
    if (!g_fbro_context_ready) {
        MessageBoxW(g_main_window, L"FBro context is not initialized yet.", L"Native FBro Demo", MB_ICONINFORMATION);
        return;
    }
    if (g_browser) {
        LogLine(L"Embedded browser already exists.");
        return;
    }
    if (!g_embed_host) {
        LogLine(L"CreateEmbeddedBrowser failed: embed host is null.");
        return;
    }

    RECT rc{};
    GetClientRect(g_embed_host, &rc);

    E_WINDOWS_INFO window_info{};
    window_info.is_null = FALSE;
    window_info.parent_window = g_embed_host;
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

    CefRefPtr<DemoBrowserEvent> event = new DemoBrowserEvent();
    CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
    extra->SetString("flag", "NativeFBroEmbedded");

    const BOOL ok = FBroHsCreate(
        CefString("https://www.baidu.com"),
        &window_info,
        &browser_setting,
        nullptr,
        extra,
        event,
        nullptr,
        CefString("NativeFBroEmbedded"));
    if (!ok) {
        LogLine(L"FBroHsCreate embedded browser failed.");
    }
}

class NativeChromeClient final : public CefClient,
                                 public CefLifeSpanHandler,
                                 public CefLoadHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        g_native_browser = browser;
        LogLine(L"Native CEF browser created.");
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser == g_native_browser) {
            g_native_browser = nullptr;
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>,
                   CefRefPtr<CefFrame> frame,
                   int http_status_code) override {
        if (frame && frame->IsMain()) {
            LogLine(L"Native CEF main frame load ended, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(NativeChromeClient);
};

void CreateNativeChromeBrowser() {
    if (!g_fbro_context_ready) {
        MessageBoxW(g_main_window, L"FBro context is not initialized yet.", L"Native FBro Demo", MB_ICONINFORMATION);
        return;
    }

    if (g_native_browser) {
        LogLine(L"Native CEF browser already exists.");
        return;
    }

    CefWindowInfo window_info;
    window_info.SetAsPopup(nullptr, L"Native CEF Chrome Browser");

    CefBrowserSettings browser_settings;
    browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);

    CefRefPtr<NativeChromeClient> client = new NativeChromeClient();
    g_native_clients.push_back(client);
    const bool ok = CefBrowserHost::CreateBrowser(
        window_info,
        client,
        CefString("https://www.baidu.com"),
        browser_settings,
        nullptr,
        nullptr);
    if (!ok) {
        LogLine(L"CefBrowserHost::CreateBrowser native browser failed.");
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_embed_button = CreateWindowExW(0, L"BUTTON",
            L"\u521b\u5efa\u5185\u5d4c\u6d4f\u89c8\u5668\uff08\u7ed1\u5b9a\u7ec4\u4ef6\u53e5\u67c4\uff09",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            16, 16, 260, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCreateEmbedded)),
            GetModuleHandleW(nullptr), nullptr);
        g_native_button = CreateWindowExW(0, L"BUTTON",
            L"\u521b\u5efa\u539f\u751f Chrome \u6d4f\u89c8\u5668",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            292, 16, 220, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCreateNative)),
            GetModuleHandleW(nullptr), nullptr);
        g_embed_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutBrowser();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kButtonCreateEmbedded) {
            CreateEmbeddedBrowser();
            return 0;
        }
        if (LOWORD(wparam) == kButtonCreateNative) {
            CreateNativeChromeBrowser();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
        LayoutBrowser();
        return 0;
    case WM_CLOSE:
        if (g_browser) {
            FBroHsBrowserHost_CloseBrowser(g_browser, true);
        }
        if (g_native_browser) {
            g_native_browser->GetHost()->CloseBrowser(true);
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

    g_main_window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kMainWindowClass,
        L"Native FBro Demo",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1200,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!g_main_window) {
        return false;
    }

    ShowWindow(g_main_window, show_cmd);
    UpdateWindow(g_main_window);
    return true;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_cmd) {
    setlocale(LC_CTYPE, "");

    const auto app_dir = ExeDir();
    SetCurrentDirectoryW(app_dir.c_str());

    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa_data{};
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed.", L"Native FBro Demo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) {
            CoUninitialize();
        }
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"Native FBro Demo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) {
            CoUninitialize();
        }
        return 1;
    }

    FBroHsSetProcessDPI(0);

    const auto subprocess = app_dir / L"FBroSubprocess.exe";
    const auto cache_dir = app_dir / L"CacheData" / L"GlobalData";
    const auto log_file = app_dir / L"debug.log";
    const auto locales_dir = app_dir / L"locales";

    const auto app_dir_ansi = ToSystemAnsi(app_dir.wstring());
    const auto subprocess_ansi = ToSystemAnsi(subprocess.wstring());
    const auto cache_ansi = ToSystemAnsi(cache_dir.wstring());
    const auto log_ansi = ToSystemAnsi(log_file.wstring());
    const auto locales_ansi = ToSystemAnsi(locales_dir.wstring());

    const bool license_ready = ApplyVipLicenseFromEnv();
    LogLine(L"LicenseReadyBeforeInit=" + std::to_wstring(license_ready ? 1 : 0));
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
    settings.log_file = const_cast<char*>(log_ansi.c_str());
    settings.log_severity = LOGSEVERITY_DEFAULT;
    settings.resources_dir_path = const_cast<char*>(app_dir_ansi.c_str());
    settings.locales_dir_path = const_cast<char*>(locales_ansi.c_str());
    settings.remote_debugging_port = 9222;
    settings.enable_auto_multiple = TRUE;

    CefRefPtr<DemoInitEvent> init_event = new DemoInitEvent();
    if (!FBroHsInitPro(&settings, init_event, 1024)) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"Native FBro Demo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) {
            CoUninitialize();
        }
        return 1;
    }

    FBroRunMessageLoop();
    FBroShutdown(TRUE);
    WSACleanup();
    if (SUCCEEDED(co_result)) {
        CoUninitialize();
    }
    return 0;
}
