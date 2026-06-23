#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroString.h"
#include "FBroSynEventDis.h"

#include <algorithm>
#include <array>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"SyncHelperDemo.MainWindow";
constexpr int kButtonSyncJs = 1001;
constexpr int kButtonSyncSource = 1002;
constexpr int kButtonSyncText = 1003;
constexpr int kButtonSyncCreateBrowser = 1004;
constexpr int kToolbarHeight = 64;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT kMsgShowResult = WM_APP + 302;
constexpr UINT kMsgReadJsResult = WM_APP + 303;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT_PTR kReadJsResultTimer = 402;
constexpr UINT kCloseFallbackMs = 5000;
constexpr DWORD kSyncTimeoutMs = 30000;

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
HWND g_sync_browser_host = nullptr;
HWND g_result_box = nullptr;
std::vector<HWND> g_buttons;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefBrowser> g_sync_browser;
bool g_fbro_ready = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;
std::vector<CefRefPtr<CefStringVisitor>> g_pending_string_visitors;

void LayoutChildren();
void ExecuteJsWithCefFallback(const std::wstring& reason);

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
    std::ofstream out(ExeDir() / L"sync-helper-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

std::wstring TruncateText(std::wstring text, size_t max_chars = 1200) {
    if (text.size() <= max_chars) {
        return text;
    }
    text.resize(max_chars);
    text += L"\n\n...";
    return text;
}

void Notify(const std::wstring& title, const std::wstring& text) {
    LogLine(title + L": " + text);
    if (g_result_box) {
        const std::wstring display = title + L"\r\n" + text;
        SetWindowTextW(g_result_box, display.c_str());
    } else {
        MessageBoxW(g_main_window, text.c_str(), title.c_str(), MB_ICONINFORMATION);
    }
}

struct ResultMessage {
    std::wstring title;
    std::wstring text;
};

void PostResult(std::wstring title, std::wstring text) {
    auto* result = new ResultMessage{std::move(title), std::move(text)};
    if (g_main_window) {
        PostMessageW(g_main_window, kMsgShowResult, 0,
            reinterpret_cast<LPARAM>(result));
    } else {
        delete result;
    }
}

bool HasLiveResources() {
    return g_browser != nullptr || g_sync_browser != nullptr;
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
    if (g_sync_browser) {
        FBroHsBrowserHost_CloseBrowser(g_sync_browser, true);
    }
    if (!g_fbro_shutdown_started) {
        g_fbro_shutdown_started = true;
        FBroShutdown(FALSE);
    }
    MaybeDestroyAfterClose();
}

class AsyncStringVisitor final : public CefStringVisitor {
public:
    explicit AsyncStringVisitor(std::wstring title) : title_(std::move(title)) {}

    void Visit(const CefString& string) override {
        LogLine(title_ + L" callback invoked.");
        PostResult(title_, TruncateText(FromCefString(string)));
    }

private:
    std::wstring title_;
    IMPLEMENT_REFCOUNTING(AsyncStringVisitor);
};

class JsResultTextVisitor final : public CefStringVisitor {
public:
    void Visit(const CefString& string) override {
        const std::wstring text = FromCefString(string);
        const std::wstring begin = L"__FBRO_SYNC_JS_RESULT__";
        const std::wstring end = L"__END__";
        const size_t start = text.find(begin);
        if (start == std::wstring::npos) {
            PostResult(L"\u540c\u6b65\u6267\u884cJS", L"\u672a\u627e\u5230 JS \u8fd4\u56de\u503c\u6807\u8bb0\u3002");
            return;
        }
        const size_t value_start = start + begin.size();
        const size_t value_end = text.find(end, value_start);
        if (value_end == std::wstring::npos) {
            PostResult(L"\u540c\u6b65\u6267\u884cJS", L"JS \u8fd4\u56de\u503c\u6807\u8bb0\u4e0d\u5b8c\u6574\u3002");
            return;
        }

        PostResult(L"\u540c\u6b65\u6267\u884cJS", text.substr(value_start, value_end - value_start));
    }

private:
    IMPLEMENT_REFCOUNTING(JsResultTextVisitor);
};

class VolcanoSyncEvent final {
public:
    VolcanoSyncEvent() {
        event_ = reinterpret_cast<SynEventDis*>(FBroSynEventDis_CreatEvent());
    }

    ~VolcanoSyncEvent() {
        if (event_) {
            FBroSynEventDis_CloseEvent(event_);
            event_ = nullptr;
        }
    }

    bool IsValid() const {
        return event_ && FBroSynEventDis_IsValid(event_) == TRUE;
    }

    void Reset() {
        if (event_) FBroSynEventDis_ResetEvent(event_);
    }

    int Wait(int timeout_ms) {
        if (!event_) return 0;
        return FBroSynEventDis_WaitEvent(event_, timeout_ms);
    }

    void Signal() {
        if (event_) FBroSynEventDis_SetEvent(event_);
    }

    void SetFrame(CefRefPtr<CefFrame> frame) {
        if (event_ && frame) FBroSynEventDis_SetCefFrame(event_, frame.get());
    }

    void SetInt(int value) {
        if (event_) FBroSynEventDis_SetIntData(event_, value);
    }

    void SetDouble(double value) {
        if (event_) FBroSynEventDis_SetDoubleData(event_, value);
    }

    void SetBool(bool value) {
        if (event_) FBroSynEventDis_SetBoolData(event_, value ? TRUE : FALSE);
    }

    void SetString(const CefString& value) {
        if (event_) FBroSynEventDis_SetWStringData(event_, value);
    }

    std::wstring DescribeValue() const {
        if (!event_) return L"Invalid sync event.";
        if (FBroSynEventDis_HavWStringData(event_)) {
            return FromFBroString(FBroSynEventDis_GetWStringData(event_));
        }
        if (FBroSynEventDis_HavIntData(event_)) {
            return std::to_wstring(FBroSynEventDis_GetIntData(event_));
        }
        if (FBroSynEventDis_HavDoubleData(event_)) {
            std::wostringstream out;
            out << FBroSynEventDis_GetDoubleData(event_);
            return out.str();
        }
        if (FBroSynEventDis_HavBoolData(event_)) {
            return FBroSynEventDis_GetBoolData(event_) ? L"true" : L"false";
        }
        return L"Callback fired, but no readable return value.";
    }

private:
    SynEventDis* event_ = nullptr;
};

class VolcanoStyleJsCallback final : public FBroHsJsCallback {
public:
    explicit VolcanoStyleJsCallback(std::shared_ptr<VolcanoSyncEvent> sync)
        : sync_(std::move(sync)) {
        type_ = JsCallbackType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) {
        if (ptr) FBroMallocManger_Free(ptr);
    }

    void Callback(CefRefPtr<CefListValue> values) override {
        if (!sync_) return;

        try {
            if (!values || values->GetSize() <= 1) {
                sync_->SetString(CefString(L"Empty JS callback values."));
                sync_->Signal();
                return;
            }

            const int value_type = values->GetInt(1);
            if (value_type == 2 && values->GetSize() > 2) {
                sync_->SetString(values->GetString(2));
            } else if (value_type == 3 && values->GetSize() > 2) {
                sync_->SetBool(values->GetBool(2));
            } else if (value_type == 1 && values->GetSize() > 2) {
                sync_->SetInt(values->GetInt(2));
            } else if (value_type == 4 && values->GetSize() > 2) {
                sync_->SetDouble(values->GetDouble(2));
            } else if (value_type == -1 && values->GetSize() > 3) {
                sync_->SetString(values->GetString(3));
            } else {
                sync_->SetString(CefString(
                    L"Unknown JS return type: " + std::to_wstring(value_type)));
            }
        } catch (...) {
            sync_->SetString(CefString(L"JS callback parse exception."));
        }
        sync_->Signal();
    }

private:
    std::shared_ptr<VolcanoSyncEvent> sync_;
    IMPLEMENT_REFCOUNTING(VolcanoStyleJsCallback);
};

class BrowserEvent final : public FBroHsBroEvent {
public:
    explicit BrowserEvent(bool sync_created = false)
        : sync_created_(sync_created) {}

    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        if (sync_created_) {
            g_sync_browser = browser;
            LogLine(L"Sync-created browser created.");
        } else {
            g_browser = browser;
            LogLine(L"Embedded Baidu browser created.");
        }
        LayoutChildren();
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser == g_browser) {
            g_browser = nullptr;
        }
        if (browser == g_sync_browser) {
            g_sync_browser = nullptr;
        }
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgMaybeDestroy, 0, 0);
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>,
                   CefRefPtr<CefFrame> frame,
                   int http_status_code) override {
        if (frame && frame->IsMain()) {
            LogLine(L"Page load ended, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    bool sync_created_;
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    const int button_width = 160;
    const int button_height = 32;
    for (size_t i = 0; i < g_buttons.size(); ++i) {
        MoveWindow(g_buttons[i], 16 + static_cast<int>(i) * (button_width + 10),
            16, button_width, button_height, TRUE);
    }

    const int browser_top = kToolbarHeight;
    const int browser_height = max(1, height - browser_top);
    const int left_width = g_sync_browser ? max(1, width / 2) : width;
    const int right_width = max(1, width - left_width);

    if (g_browser_host) {
        MoveWindow(g_browser_host, 0, browser_top, left_width,
            browser_height, TRUE);
    }
    if (g_sync_browser_host) {
        MoveWindow(g_sync_browser_host, left_width, browser_top, right_width,
            browser_height, TRUE);
        ShowWindow(g_sync_browser_host, g_sync_browser ? SW_SHOW : SW_HIDE);
    }
    if (g_result_box) {
        MoveWindow(g_result_box, max(0, width - 430), 8, 410, 48, TRUE);
    }

    auto move_browser = [](CefRefPtr<CefBrowser> browser, HWND host) {
        if (!browser || !host) return;
        HWND browser_hwnd = FBroHsBrowserHost_GetWindowHandle(browser);
        if (!browser_hwnd) return;
        RECT host_rc{};
        GetClientRect(host, &host_rc);
        MoveWindow(browser_hwnd, 0, 0,
            max(1, host_rc.right - host_rc.left),
            max(1, host_rc.bottom - host_rc.top), TRUE);
    };

    move_browser(g_browser, g_browser_host);
    move_browser(g_sync_browser, g_sync_browser_host);
}

CefRefPtr<CefFrame> MainFrame() {
    if (!g_browser) {
        return nullptr;
    }
    return FBroHsBrowser_GetMainFrame(g_browser);
}

void SyncExecuteJs() {
    ExecuteJsWithCefFallback(
        L"Skip FBro ExecuteJavaScriptToHasReturn in native C++; it aborts without Volcano CVolObject runtime.");
}

void ExecuteJsWithCefFallback(const std::wstring& reason) {
    auto frame = MainFrame();
    if (!frame) {
        Notify(L"\u540c\u6b65\u6267\u884cJS", L"\u4e3b\u6d4f\u89c8\u5668\u6846\u67b6\u8fd8\u6ca1\u6709\u51c6\u5907\u597d\u3002");
        return;
    }

    if (!reason.empty()) {
        LogLine(reason);
    }
    LogLine(L"Submit native CEF ExecuteJavaScript.");
    static const wchar_t script[] = LR"JS(
(function () {
  var result = "JS\u8fd4\u56de\u503c: " + document.title + " | " + location.href + " | " + new Date().toLocaleTimeString();
  document.title = "JS executed: " + new Date().toLocaleTimeString();
  var old = document.getElementById("sync-helper-js-result");
  if (old) old.remove();
  var node = document.createElement("div");
  node.id = "sync-helper-js-result";
  node.textContent = "__FBRO_SYNC_JS_RESULT__" + result + "__END__";
  node.style.cssText = "position:fixed;left:20px;bottom:20px;z-index:2147483647;background:#fff4cc;border:1px solid #d6a300;padding:8px 12px;font:14px sans-serif;color:#111;";
  document.body.appendChild(node);
})();
)JS";
    frame->ExecuteJavaScript(CefString(script), CefString("sync-helper-native-js"), 1);
    SetTimer(g_main_window, kReadJsResultTimer, 300, nullptr);
}

void ReadJsResult() {
    KillTimer(g_main_window, kReadJsResultTimer);
    auto frame = MainFrame();
    if (!frame) {
        Notify(L"\u540c\u6b65\u6267\u884cJS", L"\u4e3b\u6d4f\u89c8\u5668\u6846\u67b6\u8fd8\u6ca1\u6709\u51c6\u5907\u597d\u3002");
        return;
    }

    CefRefPtr<JsResultTextVisitor> visitor = new JsResultTextVisitor();
    g_pending_string_visitors.push_back(visitor);
    frame->GetText(visitor);
}

void SyncGetSource() {
    auto frame = MainFrame();
    if (!frame) {
        Notify(L"\u540c\u6b65\u53d6\u6e90\u7801", L"\u4e3b\u6d4f\u89c8\u5668\u6846\u67b6\u8fd8\u6ca1\u6709\u51c6\u5907\u597d\u3002");
        return;
    }

    CefRefPtr<AsyncStringVisitor> visitor = new AsyncStringVisitor(L"\u540c\u6b65\u53d6\u6e90\u7801");
    g_pending_string_visitors.push_back(visitor);
    LogLine(L"Submit native CEF GetSource.");
    frame->GetSource(visitor);
}

void SyncGetText() {
    auto frame = MainFrame();
    if (!frame) {
        Notify(L"\u540c\u6b65\u53d6\u6587\u672c", L"\u4e3b\u6d4f\u89c8\u5668\u6846\u67b6\u8fd8\u6ca1\u6709\u51c6\u5907\u597d\u3002");
        return;
    }

    CefRefPtr<AsyncStringVisitor> visitor = new AsyncStringVisitor(L"\u540c\u6b65\u53d6\u6587\u672c");
    g_pending_string_visitors.push_back(visitor);
    LogLine(L"Submit native CEF GetText.");
    frame->GetText(visitor);
}

void SyncCreateBrowser() {
    if (!g_fbro_ready || !g_sync_browser_host) {
        Notify(L"\u540c\u6b65\u521b\u5efa\u6d4f\u89c8\u5668", L"FBro \u6216\u6d4f\u89c8\u5668\u5bb9\u5668\u8fd8\u6ca1\u6709\u51c6\u5907\u597d\u3002");
        return;
    }
    if (g_sync_browser) {
        Notify(L"\u540c\u6b65\u521b\u5efa\u6d4f\u89c8\u5668", L"\u540c\u6b65\u521b\u5efa\u7684\u6d4f\u89c8\u5668\u5df2\u7ecf\u5b58\u5728\u3002");
        return;
    }

    RECT rc{};
    GetClientRect(g_sync_browser_host, &rc);

    E_WINDOWS_INFO window_info{};
    window_info.is_null = FALSE;
    window_info.parent_window = g_sync_browser_host;
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

    CefRefPtr<BrowserEvent> event = new BrowserEvent(true);
    CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
    extra->SetString("SyncHelperKind", "SyncCreatedBrowser");

    CefRefPtr<CefBrowser> browser = FBroHsCreateSync(
        CefString("https://www.qq.com"),
        &window_info,
        &browser_setting,
        nullptr,
        extra,
        event,
        nullptr,
        CefString("SyncCreatedBrowser"));
    g_sync_browser = browser;
    LayoutChildren();
    Notify(L"\u540c\u6b65\u521b\u5efa\u6d4f\u89c8\u5668", g_sync_browser ? L"\u540c\u6b65\u8fd4\u56de\u6d4f\u89c8\u5668\u6210\u529f\u3002" : L"\u540c\u6b65\u8fd4\u56de\u6d4f\u89c8\u5668\u4e3a\u7a7a\u3002");
}

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
    extra->SetString("SyncHelperKind", "MainBaidu");

    const BOOL ok = FBroHsCreate(CefString("https://www.baidu.com"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("SyncHelperMainBaidu"));
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
        CreateButton(hwnd, L"\u540c\u6b65\u6267\u884cJS", kButtonSyncJs);
        CreateButton(hwnd, L"\u540c\u6b65\u53d6\u6e90\u7801", kButtonSyncSource);
        CreateButton(hwnd, L"\u540c\u6b65\u53d6\u6587\u672c", kButtonSyncText);
        CreateButton(hwnd, L"\u540c\u6b65\u521b\u5efa\u6d4f\u89c8\u5668", kButtonSyncCreateBrowser);
        g_result_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        g_sync_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, kToolbarHeight, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        LogLine(L"WM_COMMAND id=" + std::to_wstring(LOWORD(wparam)));
        switch (LOWORD(wparam)) {
        case kButtonSyncJs:
            SyncExecuteJs();
            return 0;
        case kButtonSyncSource:
            SyncGetSource();
            return 0;
        case kButtonSyncText:
            SyncGetText();
            return 0;
        case kButtonSyncCreateBrowser:
            SyncCreateBrowser();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case kMsgShowResult:
        if (auto* result = reinterpret_cast<ResultMessage*>(lparam)) {
            Notify(result->title, result->text);
            delete result;
        }
        return 0;
    case WM_TIMER:
        if (wparam == kReadJsResultTimer) {
            ReadJsResult();
            return 0;
        }
        if (wparam == kCloseFallbackTimer && g_close_requested) {
            LogLine(L"Close fallback timer fired.");
            g_browser = nullptr;
            g_sync_browser = nullptr;
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
        L"\u540c\u6b65\u8f85\u52a9\u7c7b",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1320, 860,
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"SyncHelperDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"SyncHelperDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"SyncHelperDemo", MB_ICONERROR);
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
