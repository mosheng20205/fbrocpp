#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroCommand.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsBaseEvent.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroMiddleData.h"
#include "FBroString.h"
#include "FBroVIPStruct.h"
#include "FBroVIPControl.h"
#include "FBroVIPInterface.h"
#include "FBroVIPUserAgentData.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"VIPFingerprintDemo.MainWindow";
constexpr char kFingerprintUrl[] = "https://gongjux.com/fingerprint/";
constexpr int kButtonRefreshFingerprint = 1001;
constexpr int kButtonGetFingerCount = 1002;
constexpr int kButtonClearFingerCount = 1003;
constexpr int kButtonTamperBytes = 1004;
constexpr int kButtonReload = 1005;
constexpr int kToolbarHeight = 72;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT kMsgAppendLog = WM_APP + 302;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
HWND g_status_box = nullptr;
HWND g_buttons[5]{};
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<FBroHsInitEvent> g_init_event;
bool g_fbro_ready = false;
bool g_license_ready = false;
bool g_fingerprint_profile_applied = false;
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

std::filesystem::path ExePath() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len));
}

std::filesystem::path ExeDir() {
    return ExePath().parent_path();
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

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(CP_UTF8, flags,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (size <= 0) return {};

    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, flags, value.data(),
        static_cast<int>(value.size()), result.data(), size);
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

std::string Trim(std::string value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
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
    if (env_file.empty()) return {};

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

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"vip-fingerprint-demo.log",
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

int RandomInt(int min_value, int max_value) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(min_value, max_value);
    return dist(rng);
}

double RandomDouble(double min_value, double max_value) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(min_value, max_value);
    return dist(rng);
}

std::wstring RandomId(const wchar_t* prefix) {
    return std::wstring(prefix) + L"-" + std::to_wstring(RandomInt(10000, 99999));
}

CefRefPtr<FBroDoubleString> MakeBrands() {
    CefRefPtr<FBroDoubleString> brands = FBroDoubleString_Creat();
    FBroDoubleString_Add(brands, CefString("Chromium"), CefString("117"));
    FBroDoubleString_Add(brands, CefString("FBrowser"), CefString("117"));
    FBroDoubleString_Add(brands, CefString("Not-A.Brand"), CefString("9"));
    return brands;
}

CefRefPtr<FBroDoubleString> MakeFullVersionList() {
    CefRefPtr<FBroDoubleString> versions = FBroDoubleString_Creat();
    FBroDoubleString_Add(versions, CefString("Chromium"), CefString("117.0.4606.71"));
    FBroDoubleString_Add(versions, CefString("FBrowser"), CefString("117.0.4606.71"));
    FBroDoubleString_Add(versions, CefString("Not-A.Brand"), CefString("9.0.0.0"));
    return versions;
}

CefRefPtr<FBroCefStringList> MakeFormFactors() {
    CefRefPtr<FBroCefStringList> factors = FBroCefStringList_Creat();
    FBroCefStringList_Add(factors, CefString("Desktop"));
    FBroCefStringList_Add(factors, CefString("test"));
    return factors;
}

std::string MakePluginJson() {
    std::ostringstream out;
    out << "{\"data\":[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) out << ",";
        out << "{"
            << "\"name\":\"NativePlugin" << RandomInt(10, 999) << "\","
            << "\"filename\":\"plugin" << RandomInt(100, 999) << ".dll\","
            << "\"description\":\"native fbro plugin sample\","
            << "\"may_use_external_handler\":\"true\","
            << "\"mimeinfo\":[{\"mime_types\":\"application/pdf\",\"description\":\"\",\"suffixes\":\"pdf\"}]"
            << "}";
    }
    out << "]}";
    return out.str();
}

CefRefPtr<FBroVIPControl> GetVipControl() {
    if (!g_browser) return nullptr;
    CefRefPtr<FBroVIPControl> vip = FBroHsBrowser_GetVIPControl(g_browser);
    if (!vip || FBroHsVIPControl_IsNULL(vip)) {
        return nullptr;
    }
    return vip;
}

bool ApplyFingerprintProfile(bool randomize, std::wstring* summary) {
    CefRefPtr<FBroVIPControl> vip = GetVipControl();
    if (!vip) {
        if (summary) *summary = L"VIP control is null.";
        return false;
    }

    const int chrome_major = randomize ? RandomInt(112, 125) : 117;
    const int seed = randomize ? RandomInt(1, 1000) : 128;
    const int width = randomize ? RandomInt(1366, 2560) : 1920;
    const int height = randomize ? RandomInt(768, 1440) : 1080;
    const int hardware = randomize ? RandomInt(4, 16) : 8;
    const int memory = randomize ? RandomInt(4, 32) : 8;
    const double dpr = randomize ? (RandomInt(100, 250) / 100.0) : 1.25;

    CefRefPtr<FBroVIPUserAgentData> ua = FBroHsVIPUserAgentData_Create();
    const std::wstring ua_text =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/" + std::to_wstring(chrome_major) +
        L".0.4606.71 Safari/537.36 Core/1.94.235.400 QQBrowser/12.4.5599.400";
    FBroHsVIPUserAgentData_SetMainUserAgent(ua, CefString(ua_text));
    FBroHsVIPUserAgentData_SetMainAcceptLanguage(ua, CefString("zh-CN,zh,en-US,en"));
    FBroHsVIPUserAgentData_SetMainPlatform(ua, CefString("Win64"));
    FBroHsVIPUserAgentData_SetPlatform(ua, CefString("Windows"));
    FBroHsVIPUserAgentData_SetPlatformVersion(ua, CefString("10.0.0"));
    FBroHsVIPUserAgentData_SetArchitecture(ua, CefString("x86"));
    FBroHsVIPUserAgentData_SetBitness(ua, CefString("64"));
    FBroHsVIPUserAgentData_SetFullVersion(ua,
        CefString(std::to_wstring(chrome_major) + L".0.4606.71"));
    FBroHsVIPUserAgentData_SetMobile(ua, FALSE);
    FBroHsVIPUserAgentData_SetWow64(ua, FALSE);
    FBroHsVIPUserAgentData_SetBrands(ua, MakeBrands());
    FBroHsVIPUserAgentData_SetFullVersionList(ua, MakeFullVersionList());
    FBroHsVIPUserAgentData_SetFormFactors(ua, MakeFormFactors());

    FBroHsVIPControl_SetDisableDebugger(vip, TRUE);
    FBroHsVIPControl_SetVirUserAgent(vip, ua);
    FBroHsVIPControl_SetVirProductSub(vip, CefString("20030107"));
    FBroHsVIPControl_SetVirVendor(vip, CefString("FBrowser Inc."));
    FBroHsVIPControl_SetVirLanguages(vip, CefString("zh-CN,zh,en-US,en"));
    FBroHsVIPControl_SetVirAppCodeName(vip, CefString("Mozilla"));
    FBroHsVIPControl_SetVirAppName(vip, CefString("Netscape"));
    FBroHsVIPControl_SetVirAppVersion(vip, CefString("5.0"));
    FBroHsVIPControl_SetVirProduct(vip, CefString("Gecko"));
    FBroHsVIPControl_SetVirPlatform(vip, CefString("Win32"));
    FBroHsVIPControl_SetVirHardwareConcurrency(vip, hardware);
    FBroHsVIPControl_SetVirCookieEnabled(vip, TRUE);
    FBroHsVIPControl_SetVirDeviceMemory(vip, memory);
    FBroHsVIPControl_SetVirJavaEnabled(vip, TRUE);
    FBroHsVIPControl_SetVirWebdriver(vip, FALSE);
    FBroHsVIPControl_SetVirOnLine(vip, TRUE);

    const std::wstring canvas = FromFBroString(
        FBroHsVIPControl_SetCanvasFingerPrint_random(vip, 1, 16, seed));
    const std::wstring webgl = FromFBroString(
        FBroHsVIPControl_SetWebGLFingerPrint_random(vip, 1, 100, seed + 17));
    const std::wstring audio = FromFBroString(
        FBroHsVIPControl_SetAudioFingerPrint_random(vip, 100, 1000, seed + 31));

    FBroHsVIPControl_SetPlugins(vip, 2, CefString(MakePluginJson()));
    FBroHsVIPControl_SetVirCanvas2DFontFingerprint(vip, RandomDouble(-1.0, 1.0));
    FBroHsVIPControl_SetVirCSSFontFingerprint(vip,
        CefString("Arial,Calibri,Consolas,Microsoft YaHei,SimSun,Segoe UI"),
        RandomInt(0, 100), RandomInt(0, 100));
    FBroHsVIPControl_SetVirScreenHeightAndWidth(vip, height, width);
    FBroHsVIPControl_SetVirScreenavailHeightAndWidth(vip, height - 40, width - 10);
    FBroHsVIPControl_SetVirViewport(vip, 0, 0, width, height);
    FBroHsVIPControl_SetVirScreencolorDepth(vip, 24);
    FBroHsVIPControl_SetVirScreenpixelDepth(vip, 24);
    FBroHsVIPControl_SetVirDevicePixelRatio(vip, dpr);
    FBroHsVIPControl_SetVirWebglvendor(vip, CefString("Intel Inc."));
    FBroHsVIPControl_SetVirWebglrenderer(vip, CefString("Intel(R) Iris(R) Xe Graphics"));
    FBroHsVIPControl_SetVirWebrtcIP(vip, CefString("172.236.241.238"),
        CefString("192.168." + std::to_string(RandomInt(1, 254)) + "." +
            std::to_string(RandomInt(1, 254))),
        CefString(""), FALSE);
    FBroHsVIPControl_SetVirTimeZone(vip, -8, 0,
        CefString("PST"), CefString("America/Los_Angeles"));
    FBroHsVIPControl_SetTouchEventEmulationEnabled(vip, TRUE, RandomInt(1, 10));
    FBroHsVIPControl_SetVirBatteryManagerCharging(vip, TRUE);
    FBroHsVIPControl_SetVirBatteryManagerLevel(vip, RandomInt(30, 99) / 100.0);
    FBroHsVIPControl_SetVirAudioInput(vip,
        CefString("{\"label\":\"Microphone Array (Realtek Audio)\",\"deviceId\":\"" +
            ToUtf8(RandomId(L"audio-input")) + "\"}"));
    FBroHsVIPControl_SetVirVideoInput(vip,
        CefString("{\"label\":\"Integrated Camera\",\"deviceId\":\"" +
            ToUtf8(RandomId(L"video-input")) + "\"}"));
    FBroHsVIPControl_SetVirAudioOutput(vip,
        CefString("{\"label\":\"Speakers (High Definition Audio)\",\"deviceId\":\"" +
            ToUtf8(RandomId(L"audio-output")) + "\"}"));
    FBroHsVIPControl_SetVirOrientation(vip, 1, 90);
    FBroHsVIPControl_SetDisablePerformanceCheck(vip, TRUE, 0, 0);
    FBroHsVIPControl_SetCSSKernel(vip, chrome_major);
    FBroHsVIPControl_SetWebFeatureKernel(vip, chrome_major);
    FBroHsVIPControl_SetV8Kernel(vip, chrome_major);

    if (summary) {
        std::wostringstream out;
        out << L"fingerprint profile applied. chrome=" << chrome_major
            << L", seed=" << seed
            << L", screen=" << width << L"x" << height
            << L", canvas=" << (canvas.empty() ? L"(empty)" : canvas)
            << L", webgl=" << (webgl.empty() ? L"(empty)" : webgl)
            << L", audio=" << (audio.empty() ? L"(empty)" : audio);
        *summary = out.str();
    }
    return true;
}

bool ApplyConstantByteFingerprint(std::wstring* summary) {
    CefRefPtr<FBroVIPControl> vip = GetVipControl();
    if (!vip) {
        if (summary) *summary = L"VIP control is null.";
        return false;
    }

    const std::wstring marker = L"01 02 03 04-" + std::to_wstring(RandomInt(1000, 9999));
    FBroHsVIPControl_SetCanvasFingerPrint_constant(vip, CefString(L"canvas-bytes:" + marker));
    FBroHsVIPControl_SetWebGLFingerPrint_constant(vip, CefString(L"webgl-bytes:" + marker));
    FBroHsVIPControl_SetAudioFingerPrint_constant(vip, CefString(L"audio-bytes:" + marker));

    if (summary) {
        *summary = L"constant byte-style fingerprint data set: " + marker;
    }
    return true;
}

void ReloadFingerprintPage(bool ignore_cache) {
    if (!g_browser) {
        AppendStatusDirect(L"browser is not ready.");
        return;
    }
    if (ignore_cache) {
        FBroHsBrowser_ReloadIgnoreCache(g_browser);
    } else {
        FBroHsBrowser_Reload(g_browser);
    }
}

void RefreshFingerprint() {
    std::wstring summary;
    if (!ApplyFingerprintProfile(true, &summary)) {
        AppendStatusDirect(L"fingerprint refresh failed: " + summary);
        return;
    }
    AppendStatusDirect(L"fingerprint refreshed: " + summary);
    ReloadFingerprintPage(true);
}

void GetFingerCount() {
    CefRefPtr<FBroVIPControl> vip = GetVipControl();
    if (!vip) {
        AppendStatusDirect(L"VIP control is null.");
        return;
    }
    const std::wstring count = FromFBroString(FBroHsVIPControl_GetFingerCount(vip));
    AppendStatusDirect(L"finger count: " + (count.empty() ? L"(empty)" : count));
}

void ClearFingerCount() {
    CefRefPtr<FBroVIPControl> vip = GetVipControl();
    if (!vip) {
        AppendStatusDirect(L"VIP control is null.");
        return;
    }
    FBroHsVIPControl_ClearFingerCount(vip);
    const std::wstring count = FromFBroString(FBroHsVIPControl_GetFingerCount(vip));
    AppendStatusDirect(L"finger count cleared. current: " +
        (count.empty() ? L"(empty)" : count));
}

void TamperByteFingerprint() {
    std::wstring summary;
    if (!ApplyConstantByteFingerprint(&summary)) {
        AppendStatusDirect(L"byte-style tamper failed: " + summary);
        return;
    }
    AppendStatusDirect(summary);
    ReloadFingerprintPage(true);
}

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    const int button_width = 140;
    const int button_height = 30;
    const int x0 = 12;
    const int y0 = 12;
    for (int i = 0; i < 5; ++i) {
        if (g_buttons[i]) {
            MoveWindow(g_buttons[i], x0 + i * (button_width + 8), y0,
                button_width, button_height, TRUE);
        }
    }

    if (g_status_box) {
        const int status_x = x0;
        MoveWindow(g_status_box, status_x, y0 + button_height + 8,
            MaxValue(1, width - status_x - 12), 24, TRUE);
    }

    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, kToolbarHeight, width,
            MaxValue(1, height - kToolbarHeight), TRUE);
    }

    if (g_browser && g_browser_host) {
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(g_browser);
        if (browser_hwnd) {
            RECT host_rc{};
            GetClientRect(g_browser_host, &host_rc);
            MoveWindow(browser_hwnd, 0, 0,
                MaxValue(1L, host_rc.right - host_rc.left),
                MaxValue(1L, host_rc.bottom - host_rc.top), TRUE);
        }
    }
}

class BrowserEvent final : public FBroHsBroEvent {
public:
    BrowserEvent() {
        type_ = BroEventType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) {
            FBroMallocManger_Free(ptr);
        }
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        std::wstring summary;
        g_fingerprint_profile_applied = ApplyFingerprintProfile(false, &summary);
        PostStatus(L"embedded VIP fingerprint browser created. " + summary);

        CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(browser);
        if (frame) {
            FBroHsBrowserFrame_LoadURL(frame, CefString(kFingerprintUrl));
        }
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
            PostStatus(L"page load ended. status=" +
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
    extra->SetString("flag", "VIPFingerprintDemo");

    const BOOL ok = FBroHsCreate(CefString("about:blank"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("VIPFingerprintDemo"));
    if (!ok) {
        AppendStatusDirect(L"FBroHsCreate failed.");
    }
}

class InitEvent final : public FBroHsInitEvent {
public:
    InitEvent() {
        type_ = InitEventType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) {
            FBroMallocManger_Free(ptr);
        }
    }

    void OnContextInitialized() override {
        g_fbro_ready = true;
        PostStatus(L"FBro context initialized.");
        CreateEmbeddedBrowser();
    }

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        FBroHsCommandLine_DisableGpuBlockList(command_line);
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
};

void HandleButton(int id) {
    switch (id) {
    case kButtonRefreshFingerprint:
        RefreshFingerprint();
        break;
    case kButtonGetFingerCount:
        GetFingerCount();
        break;
    case kButtonClearFingerCount:
        ClearFingerCount();
        break;
    case kButtonTamperBytes:
        TamperByteFingerprint();
        break;
    case kButtonReload:
        ReloadFingerprintPage(true);
        AppendStatusDirect(L"page reloaded.");
        break;
    default:
        break;
    }
}

HWND CreateButton(HWND parent, int id, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_buttons[0] = CreateButton(hwnd, kButtonRefreshFingerprint,
            L"\u6307\u7eb9\u5237\u65b0");
        g_buttons[1] = CreateButton(hwnd, kButtonGetFingerCount,
            L"\u83b7\u53d6\u6307\u7eb9\u8c03\u7528\u6570");
        g_buttons[2] = CreateButton(hwnd, kButtonClearFingerCount,
            L"\u6e05\u7a7a\u6307\u7eb9\u8c03\u7528\u6570");
        g_buttons[3] = CreateButton(hwnd, kButtonTamperBytes,
            L"\u7be1\u6539\u5b57\u8282\u96c6\u6570\u636e");
        g_buttons[4] = CreateButton(hwnd, kButtonReload,
            L"\u91cd\u65b0\u52a0\u8f7d");

        g_status_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_status_box, WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        AppendStatusDirect(L"VIP fingerprint demo uses local .env for license.");
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        HandleButton(LOWORD(wparam));
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
    case kMsgAppendLog: {
        auto* message = reinterpret_cast<LogMessage*>(lparam);
        if (message) {
            AppendStatusDirect(message->text);
            delete message;
        }
        return 0;
    }
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
        L"VIP\u6307\u7eb9\u6d4b\u8bd5",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1320, 860,
        nullptr, nullptr, instance, nullptr);
    if (!g_main_window) return false;

    ShowWindow(g_main_window, show_cmd);
    UpdateWindow(g_main_window);
    return true;
}

bool ApplyVipLicenseFromEnv() {
    const auto key = ReadEnvValue("FBRO_VIP_LICENSE_KEY");
    if (key.empty()) {
        LogLine(L"No FBRO_VIP_LICENSE_KEY found in local .env.");
        return false;
    }

    const auto process_type = FBroGetProcessType();
    if (process_type != BrowserProcess) {
        return true;
    }

    struct SetKeyState {
        std::atomic_bool done{false};
        BOOL result{FALSE};
    };

    auto state = std::make_shared<SetKeyState>();
    std::thread([key, state]() {
        state->result = FBroHsOnlineLicenseControl_SetKey(CefString(FromUtf8(key)));
        state->done = true;
    }).detach();

    for (int i = 0; i < 300; ++i) {
        if (state->done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!state->done) {
        LogLine(L"FBroHsOnlineLicenseControl_SetKey did not return within 30 seconds.");
        return false;
    }

    LogLine(L"VIP license key loaded from .env. SetKeyResult=" +
        std::to_wstring(state->result));
    return state->result == TRUE;
}

bool IsCefSubprocessCommandLine() {
    const wchar_t* command_line = GetCommandLineW();
    return command_line && wcsstr(command_line, L"--type=") != nullptr;
}

bool InitFbro() {
    const auto app_dir = ExeDir();
    const auto subprocess = ExePath();
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
    settings.multi_threaded_message_loop = TRUE;
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

    g_init_event = new InitEvent();
    return FBroHsInitPro(&settings, g_init_event, 1024) == TRUE;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_cmd) {
    setlocale(LC_CTYPE, "");
    SetCurrentDirectoryW(ExeDir().c_str());

    if (IsCefSubprocessCommandLine()) {
        const bool initialized = InitFbro();
        g_init_event = nullptr;
        return initialized ? 0 : 1;
    }

    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed.", L"VIPFingerprintDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"VIPFingerprintDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    g_license_ready = ApplyVipLicenseFromEnv();
    AppendStatusDirect(g_license_ready ?
        L"VIP license loaded from local .env." :
        L"VIP license not confirmed. Fingerprint APIs may fail.");

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"VIPFingerprintDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!g_fbro_shutdown_started) {
        FBroShutdown(FALSE);
    }
    g_init_event = nullptr;
    WSACleanup();
    if (SUCCEEDED(co_result)) CoUninitialize();
    return 0;
}
