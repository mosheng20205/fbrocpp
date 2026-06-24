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
#include "FBroVIPEventInterface.h"
#include "FBroVIPInterface.h"
#include "include/cef_parser.h"

#include <algorithm>
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

constexpr wchar_t kMainWindowClass[] = L"VIPAdvancedFeatureDemo.MainWindow";
constexpr char kStartUrl[] = "https://www.baidu.com";
constexpr int kToolbarWidth = 360;
constexpr int kButtonWidth = 156;
constexpr int kButtonHeight = 28;
constexpr int kLogHeight = 140;
constexpr UINT kMsgMaybeDestroy = WM_APP + 401;
constexpr UINT kMsgAppendLog = WM_APP + 402;
constexpr UINT_PTR kCloseFallbackTimer = 501;
constexpr UINT kCloseFallbackMs = 5000;

enum ButtonId {
    kBtnSendMessage = 1001,
    kBtnExecuteMethod,
    kBtnEnableObserver,
    kBtnDisableObserver,
    kBtnScreenshot,
    kBtnTouchEvent,
    kBtnTouchSwipe,
    kBtnTouchClick,
    kBtnKeyCtrlA,
    kBtnKeyText,
    kBtnMouseClick,
    kBtnMouseWheel,
    kBtnRuntimeEnable,
    kBtnRuntimeDisable,
    kBtnContextList,
    kBtnRuntimeJs,
    kBtnRuntimeFrameId,
    kBtnRuntimeMainFrame,
    kBtnRuntimeAllFrames,
    kBtnRuntimeFrameIndex,
    kBtnFilterChange,
    kBtnFilterClear,
    kBtnResourceData,
    kBtnResourceFile,
    kBtnResourceClear,
    kBtnDomDocument,
    kBtnDomSearch,
    kBtnDomRemoveAttribute,
    kBtnDomRemoveNode,
    kBtnDomSetAttributeText,
    kBtnDomSetAttributeValue,
    kBtnDomSetNodeValue,
    kBtnDomSetOuterHtml,
    kBtnDomGetAttribute,
    kBtnDomGetOuterHtml,
    kBtnDomQuerySelector,
    kBtnDomQuerySelectorAll,
    kBtnDomSetNodeName,
    kBtnDomFillForm,
};

struct ButtonSpec {
    int id;
    const wchar_t* text;
};

const ButtonSpec kButtons[] = {
    {kBtnSendMessage, L"发送消息"},
    {kBtnExecuteMethod, L"执行方法"},
    {kBtnEnableObserver, L"启用事件"},
    {kBtnDisableObserver, L"停用事件"},
    {kBtnScreenshot, L"VIP截图"},
    {kBtnTouchEvent, L"VIP触摸事件"},
    {kBtnTouchSwipe, L"触摸测试_滑动"},
    {kBtnTouchClick, L"触摸测试_点击"},
    {kBtnKeyCtrlA, L"按键测试_C+A"},
    {kBtnKeyText, L"按键测试_输入文本"},
    {kBtnMouseClick, L"鼠标测试_移动点击"},
    {kBtnMouseWheel, L"鼠标测试_滚轮"},
    {kBtnRuntimeEnable, L"VIP启用环境"},
    {kBtnRuntimeDisable, L"VIP关闭环境"},
    {kBtnContextList, L"VIP取环境清单ID"},
    {kBtnRuntimeJs, L"VIP执行JS"},
    {kBtnRuntimeFrameId, L"VIP执行JS_框架ID"},
    {kBtnRuntimeMainFrame, L"VIP执行JS_主框架"},
    {kBtnRuntimeAllFrames, L"VIP执行JS_全部框架"},
    {kBtnRuntimeFrameIndex, L"VIP执行JS_框架序号"},
    {kBtnFilterChange, L"修改内容"},
    {kBtnFilterClear, L"取消修改内容"},
    {kBtnResourceData, L"用数据替换资源"},
    {kBtnResourceFile, L"用本地文件替换资源"},
    {kBtnResourceClear, L"取消替换资源"},
    {kBtnDomDocument, L"枚举DOM_同步"},
    {kBtnDomSearch, L"查找文本_同步"},
    {kBtnDomRemoveAttribute, L"移除节点属性"},
    {kBtnDomRemoveNode, L"移除节点"},
    {kBtnDomSetAttributeText, L"置节点属性文本"},
    {kBtnDomSetAttributeValue, L"置节点属性值"},
    {kBtnDomSetNodeValue, L"置节点值"},
    {kBtnDomSetOuterHtml, L"置节点源码"},
    {kBtnDomGetAttribute, L"取节点属性值_同步"},
    {kBtnDomGetOuterHtml, L"取节点源码_同步"},
    {kBtnDomQuerySelector, L"查询节点选择器_同步"},
    {kBtnDomQuerySelectorAll, L"查询全部节点选择器_同步"},
    {kBtnDomSetNodeName, L"置节点名_同步"},
    {kBtnDomFillForm, L"模拟DOM填表"},
};

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
HWND g_log_box = nullptr;
std::vector<HWND> g_buttons;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<FBroVIPControl> g_vip_control;
CefRefPtr<FBroHsInitEvent> g_init_event;
CefRefPtr<FBroHsDevToolsMessageObserver> g_devtools_observer;
CefRefPtr<FBroHsGeneralResultCallback> g_general_callback;
bool g_fbro_ready = false;
bool g_license_ready = false;
bool g_devtools_observer_enabled = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;
int g_last_context_id = 0;
std::string g_last_frame_id;
int g_root_node_id = 0;
int g_target_node_id = 0;
std::string g_last_search_id;
int g_last_search_count = 0;
std::vector<char> g_resource_html;

struct LogMessage {
    std::wstring text;
};

template <typename T>
T MaxValue(T a, T b) {
    return a > b ? a : b;
}

template <typename T>
T MinValue(T a, T b) {
    return a < b ? a : b;
}

void LayoutChildren();
void CreateEmbeddedBrowser();

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

std::wstring FromCefString(const CefString& value) {
    return value.ToWString();
}

std::string CefStringToUtf8(const CefString& value) {
    return ToUtf8(value.ToWString());
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
    for (int i = 0; i < 8; ++i) {
        const auto candidate = dir / L".env";
        if (std::filesystem::exists(candidate)) return candidate;
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
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
    std::ofstream out(ExeDir() / L"vip-advanced-feature-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

void AppendLogDirect(const std::wstring& text) {
    LogLine(text);
    if (!g_log_box) return;
    const int len = GetWindowTextLengthW(g_log_box);
    SendMessageW(g_log_box, EM_SETSEL, len, len);
    SendMessageW(g_log_box, EM_REPLACESEL, FALSE,
        reinterpret_cast<LPARAM>((text + L"\r\n").c_str()));
}

void PostLog(std::wstring text) {
    auto* message = new LogMessage{std::move(text)};
    if (g_main_window) {
        PostMessageW(g_main_window, kMsgAppendLog, 0,
            reinterpret_cast<LPARAM>(message));
    } else {
        delete message;
    }
}

std::wstring PreviewText(const std::string& text, size_t max_len = 800) {
    std::string preview = text.substr(0, MinValue(text.size(), max_len));
    if (text.size() > max_len) preview += "...";
    return FromUtf8(preview);
}

CefRefPtr<FBroVIPControl> GetVipControl() {
    if (g_vip_control && !FBroHsVIPControl_IsNULL(g_vip_control)) {
        return g_vip_control;
    }
    if (!g_browser) {
        return nullptr;
    }
    CefRefPtr<FBroVIPControl> vip = FBroHsBrowser_GetVIPControl(g_browser);
    if (!vip || FBroHsVIPControl_IsNULL(vip)) return nullptr;
    g_vip_control = vip;
    return vip;
}

bool EnsureVip(const wchar_t* action, CefRefPtr<FBroVIPControl>* out_vip) {
    CefRefPtr<FBroVIPControl> vip = GetVipControl();
    if (!vip) {
        AppendLogDirect(std::wstring(action) + L": VIP control is null.");
        return false;
    }
    if (out_vip) *out_vip = vip;
    return true;
}

bool IsKnownButtonId(int id) {
    for (const auto& spec : kButtons) {
        if (spec.id == id) return true;
    }
    return false;
}

void ReloadPage() {
    if (g_browser) {
        FBroHsBrowser_ReloadIgnoreCache(g_browser);
    }
}

bool HasLiveResources() {
    return g_browser != nullptr;
}

void MaybeDestroyAfterClose() {
    if (!g_close_requested || g_destroying_window || HasLiveResources()) return;
    g_destroying_window = true;
    if (g_main_window) {
        KillTimer(g_main_window, kCloseFallbackTimer);
        DestroyWindow(g_main_window);
    }
    FBroQuitMessageLoop();
    PostQuitMessage(0);
}

void RequestAppClose(HWND hwnd) {
    if (g_destroying_window) return;
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

std::wstring ValueTypeName(CefValueType type) {
    switch (type) {
    case VTYPE_NULL: return L"null";
    case VTYPE_BOOL: return L"bool";
    case VTYPE_INT: return L"int";
    case VTYPE_DOUBLE: return L"double";
    case VTYPE_STRING: return L"string";
    case VTYPE_BINARY: return L"binary";
    case VTYPE_DICTIONARY: return L"dictionary";
    case VTYPE_LIST: return L"list";
    default: return L"invalid";
    }
}

void RememberContextList(CefRefPtr<CefListValue> listvalue) {
    if (!listvalue) return;
    for (size_t i = 0; i < listvalue->GetSize(); ++i) {
        if (listvalue->GetType(i) == VTYPE_DICTIONARY) {
            CefRefPtr<CefDictionaryValue> dict = listvalue->GetDictionary(i);
            if (!dict) continue;
            if (dict->HasKey("id") && dict->GetType("id") == VTYPE_INT) {
                g_last_context_id = dict->GetInt("id");
            } else if (dict->HasKey("contextId") && dict->GetType("contextId") == VTYPE_INT) {
                g_last_context_id = dict->GetInt("contextId");
            }
            if (dict->HasKey("frameId")) {
                g_last_frame_id = CefStringToUtf8(dict->GetString("frameId"));
            }
            if (g_last_context_id != 0) break;
        }
    }
}

void RememberDomData(const std::string& json) {
    CefRefPtr<CefValue> value = CefParseJSON(json, JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY) return;
    CefRefPtr<CefDictionaryValue> dict = value->GetDictionary();
    if (!dict) return;

    CefRefPtr<CefDictionaryValue> root;
    if (dict->HasKey("root")) {
        root = dict->GetDictionary("root");
    } else if (dict->HasKey("result")) {
        CefRefPtr<CefDictionaryValue> result = dict->GetDictionary("result");
        if (result && result->HasKey("root")) {
            root = result->GetDictionary("root");
        }
    }
    if (root && root->HasKey("nodeId")) {
        g_root_node_id = root->GetInt("nodeId");
    }

    if (dict->HasKey("nodeId") && dict->GetType("nodeId") == VTYPE_INT) {
        g_target_node_id = dict->GetInt("nodeId");
    }
    if (dict->HasKey("searchId")) {
        g_last_search_id = CefStringToUtf8(dict->GetString("searchId"));
    }
    if (dict->HasKey("resultCount") && dict->GetType("resultCount") == VTYPE_INT) {
        g_last_search_count = dict->GetInt("resultCount");
    }
    if (dict->HasKey("nodeIds") && dict->GetType("nodeIds") == VTYPE_LIST) {
        CefRefPtr<CefListValue> ids = dict->GetList("nodeIds");
        if (ids && ids->GetSize() > 0 && ids->GetType(0) == VTYPE_INT) {
            g_target_node_id = ids->GetInt(0);
        }
    }
}

class GeneralCallback final : public FBroHsGeneralResultCallback {
public:
    GeneralCallback() {
        type_ = GeneralResultCallbackType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) FBroMallocManger_Free(ptr);
    }

    void Callback_Data(CefRefPtr<CefBrowser>,
                       int message_id,
                       bool success,
                       const void* data,
                       size_t datasize) override {
        std::string body;
        if (data && datasize > 0) {
            body.assign(static_cast<const char*>(data),
                static_cast<const char*>(data) + datasize);
        }
        RememberDomData(body);
        std::wostringstream out;
        out << L"GeneralResult data, id=" << message_id
            << L", success=" << (success ? L"true" : L"false")
            << L", bytes=" << datasize
            << L", rootNode=" << g_root_node_id
            << L", targetNode=" << g_target_node_id
            << L", preview=" << PreviewText(body);
        PostLog(out.str());
    }

    void Callback_ListData(CefRefPtr<CefBrowser>,
                           int message_id,
                           bool success,
                           CefRefPtr<CefListValue> listvalue) override {
        RememberContextList(listvalue);
        std::wostringstream out;
        out << L"GeneralResult list, id=" << message_id
            << L", success=" << (success ? L"true" : L"false")
            << L", count=" << (listvalue ? listvalue->GetSize() : 0)
            << L", contextId=" << g_last_context_id
            << L", frameId=" << FromUtf8(g_last_frame_id);
        if (listvalue) {
            for (size_t i = 0; i < listvalue->GetSize(); ++i) {
                out << L"\r\n  [" << i << L"] " << ValueTypeName(listvalue->GetType(i));
                if (listvalue->GetType(i) == VTYPE_STRING) {
                    out << L": " << FromCefString(listvalue->GetString(i));
                }
            }
        }
        PostLog(out.str());
    }

private:
    IMPLEMENT_REFCOUNTING(GeneralCallback);
};

class DevToolsObserver final : public FBroHsDevToolsMessageObserver {
public:
    DevToolsObserver() {
        type_ = DevToolsMessageObserverType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) FBroMallocManger_Free(ptr);
    }

    bool OnDevToolsMessage(CefRefPtr<CefBrowser>,
                           const void* message,
                           size_t message_size) override {
        std::string body;
        if (message && message_size > 0) {
            body.assign(static_cast<const char*>(message),
                static_cast<const char*>(message) + message_size);
        }
        PostLog(L"DevTools message: " + PreviewText(body));
        return false;
    }

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>,
                                int message_id,
                                bool success,
                                const void* result,
                                size_t result_size) override {
        std::string body;
        if (result && result_size > 0) {
            body.assign(static_cast<const char*>(result),
                static_cast<const char*>(result) + result_size);
        }
        std::wostringstream out;
        out << L"DevTools method result, id=" << message_id
            << L", success=" << (success ? L"true" : L"false")
            << L", preview=" << PreviewText(body);
        PostLog(out.str());
    }

    void OnDevToolsEvent(CefRefPtr<CefBrowser>,
                         const CefString& method,
                         const void* params,
                         size_t params_size) override {
        std::string body;
        if (params && params_size > 0) {
            body.assign(static_cast<const char*>(params),
                static_cast<const char*>(params) + params_size);
        }
        PostLog(L"DevTools event: " + FromCefString(method) +
            L", params=" + PreviewText(body));
    }

    void OnDevToolsAgentAttached(CefRefPtr<CefBrowser>) override {
        PostLog(L"DevTools agent attached.");
    }

    void OnDevToolsAgentDetached(CefRefPtr<CefBrowser>) override {
        PostLog(L"DevTools agent detached.");
    }

private:
    IMPLEMENT_REFCOUNTING(DevToolsObserver);
};

class BrowserEvent final : public FBroHsBroEvent {
public:
    BrowserEvent() {
        type_ = BroEventType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) FBroMallocManger_Free(ptr);
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        g_vip_control = FBroHsBrowser_GetVIPControl(browser);
        if (g_vip_control && !FBroHsVIPControl_IsNULL(g_vip_control)) {
            PostLog(L"embedded VIP advanced browser created. VIP control cached.");
        } else {
            g_vip_control = nullptr;
            PostLog(L"embedded VIP advanced browser created, but VIP control is null.");
        }
        CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(browser);
        if (frame) {
            FBroHsBrowserFrame_LoadURL(frame, CefString(kStartUrl));
        }
        LayoutChildren();
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser == g_browser) {
            g_browser = nullptr;
            g_vip_control = nullptr;
        }
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgMaybeDestroy, 0, 0);
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>,
                   CefRefPtr<CefFrame> frame,
                   int http_status_code) override {
        if (frame && frame->IsMain()) {
            PostLog(L"page load ended. status=" + std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

class InitEvent final : public FBroHsInitEvent {
public:
    InitEvent() {
        type_ = InitEventType;
    }

    static void* operator new(size_t size) {
        return FBroMallocManger_New(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr) FBroMallocManger_Free(ptr);
    }

    void OnContextInitialized() override {
        g_fbro_ready = true;
        PostLog(L"FBro context initialized.");
        CreateEmbeddedBrowser();
    }

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        FBroHsCommandLine_DisableGpuBlockList(command_line);
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
};

void LayoutChildren() {
    if (!g_main_window) return;
    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int padding = 8;
    const int col_gap = 8;
    const int row_gap = 6;
    const int left_width = MinValue(kToolbarWidth, MaxValue(1, width / 3));

    for (size_t i = 0; i < g_buttons.size(); ++i) {
        const int col = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        const int x = padding + col * (kButtonWidth + col_gap);
        const int y = padding + row * (kButtonHeight + row_gap);
        MoveWindow(g_buttons[i], x, y, kButtonWidth, kButtonHeight, TRUE);
    }

    if (g_browser_host) {
        MoveWindow(g_browser_host, left_width, 0,
            MaxValue(1, width - left_width), MaxValue(1, height - kLogHeight),
            TRUE);
    }
    if (g_log_box) {
        MoveWindow(g_log_box, left_width, MaxValue(1, height - kLogHeight),
            MaxValue(1, width - left_width), kLogHeight, TRUE);
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
    extra->SetString("flag", "VIPAdvancedFeatureDemo");

    const BOOL ok = FBroHsCreate(CefString("about:blank"), &window_info,
        &browser_setting, nullptr, extra, event, nullptr,
        CefString("VIPAdvancedFeatureDemo"));
    if (!ok) {
        AppendLogDirect(L"FBroHsCreate failed.");
    }
}

void SendDevToolsMessage() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"发送消息", &vip)) return;
    FBroHsVIPControl_SendDevToolsMessage(vip, CefString("this is error message to test"));
    AppendLogDirect(L"发送消息: called FBroHsVIPControl_SendDevToolsMessage.");
}

void ExecuteDevToolsMethod() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"执行方法", &vip)) return;
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("format", "jpeg");
    params->SetInt("quality", 80);
    CefRefPtr<CefDictionaryValue> clip = CefDictionaryValue::Create();
    clip->SetInt("x", 0);
    clip->SetInt("y", 0);
    clip->SetInt("width", 1024);
    clip->SetInt("height", 768);
    clip->SetInt("scale", 1);
    params->SetDictionary("clip", clip);
    params->SetBool("fromSurface", false);
    params->SetBool("captureBeyondViewport", true);
    FBroHsVIPControl_ExecuteDevToolsMethod(vip, 20068,
        CefString("Page.captureScreenshot"), params);
    AppendLogDirect(L"执行方法: called Page.captureScreenshot.");
}

void EnableDevToolsObserver() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"启用事件", &vip)) return;
    if (!g_devtools_observer) {
        g_devtools_observer = new DevToolsObserver();
    }
    const BOOL ok = FBroHsVIPControl_AddDevToolsMessageObserver(vip, g_devtools_observer);
    g_devtools_observer_enabled = ok == TRUE;
    AppendLogDirect(L"启用事件: " + std::wstring(ok ? L"success" : L"failed"));
}

void DisableDevToolsObserver() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"停用事件", &vip)) return;
    const BOOL ok = FBroHsVIPControl_DeleteDevToolsMessageObserver(vip);
    g_devtools_observer_enabled = false;
    AppendLogDirect(L"停用事件: " + std::wstring(ok ? L"success" : L"failed"));
}

void CaptureScreenshot() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"VIP截图", &vip)) return;
    VIEWPORT viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = 1024;
    viewport.height = 1024;
    viewport.scale = 1;
    FBroHsVIPControl_PageCaptureScreenshot(vip, CefString("jpeg"), 90,
        &viewport, FALSE, TRUE, g_general_callback, nullptr, 0);
    AppendLogDirect(L"VIP截图: requested jpeg screenshot.");
}

void DispatchTouch(int type, int x, int y) {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"VIP触摸事件", &vip)) return;
    E_DEV_TOUCHPOINT point{};
    point.x = x;
    point.y = y;
    point.radiusX = 1;
    point.radiusY = 1;
    point.rotationAngle = 0;
    point.force = 1;
    point.id = 1;
    FBroHsVIPControl_DispatchTouchEvent(vip, type, &point, 0);
}

void DispatchKey(const CefString& type, int modifiers, const CefString& text,
                 int windows_key, bool is_system = false) {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"按键测试", &vip)) return;
    FBroHsVIPControl_DispatchKeyEvent(vip, type, modifiers, text, text,
        CefString(""), CefString(""), CefString(""), windows_key, 0,
        FALSE, FALSE, is_system ? TRUE : FALSE, 0);
}

void DispatchMouse(const CefString& type, int x, int y, int modifiers,
                   const CefString& button, int buttons, int click_count,
                   int delta_x, int delta_y) {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"鼠标测试", &vip)) return;
    FBroHsVIPControl_DispatchMouseEvent(vip, type, x, y, modifiers, button,
        buttons, click_count, delta_x, delta_y, CefString(""));
}

void RuntimeEval(const CefString& expression,
                 int context_id,
                 int frame_type,
                 int frame_index,
                 const CefString& frame_id,
                 const wchar_t* label) {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(label, &vip)) return;
    if (frame_type < 0) {
        FBroHsVIPControl_RuntimeEvaluate(vip, expression, TRUE, context_id,
            FALSE, TRUE, 10000, FALSE, FALSE, g_general_callback, nullptr, 0);
    } else {
        FBroHsVIPControl_RuntimeEvaluate_FrameID(vip, expression, TRUE,
            frame_type, frame_index, frame_id, FALSE, TRUE, 10000, FALSE,
            FALSE, g_general_callback, nullptr, 0);
    }
    AppendLogDirect(std::wstring(label) + L": requested.");
}

void ContextList() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"VIP取环境清单ID", &vip)) return;
    CefRefPtr<CefListValue> list = FBroHsVIPControl_PageGetContextID(vip);
    RememberContextList(list);
    std::wostringstream out;
    out << L"VIP取环境清单ID: count=" << (list ? list->GetSize() : 0)
        << L", contextId=" << g_last_context_id
        << L", frameId=" << FromUtf8(g_last_frame_id);
    AppendLogDirect(out.str());
}

void FilterChange() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"修改内容", &vip)) return;
    FBroHsVIPControl_AddResponseFilterChangeData(vip, 0,
        CefString("https://www.baidu.com/"), 1,
        CefString("百度"), CefString("FBrowserCEF3Lib"));
    ReloadPage();
    AppendLogDirect(L"修改内容: replace 百度 -> FBrowserCEF3Lib, then reload.");
}

void FilterClear() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"取消修改内容", &vip)) return;
    FBroHsVIPControl_DeletResponseFiltereChangeData(vip,
        CefString("https://www.baidu.com/"));
    ReloadPage();
    AppendLogDirect(L"取消修改内容: cleared response filter for Baidu.");
}

void ResourceData() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"用数据替换资源", &vip)) return;
    const char* html =
        "<html><head><meta charset=\"UTF-8\"><title>VIP Resource Replace</title></head>"
        "<body><h2>你好 FBrowser</h2><p>这是纯 C++ 通过 VIP ResourceHandler 替换百度首页。</p>"
        "<input id=\"kw\" value=\"native vip resource\"><button>测试按钮</button></body></html>";
    g_resource_html.assign(html, html + strlen(html));
    FBroHsVIPControl_AddResourceHandlerChangeData(vip, 0,
        CefString("https://www.baidu.com/"), CefString("text/html"),
        nullptr, g_resource_html.data(), g_resource_html.size());
    ReloadPage();
    AppendLogDirect(L"用数据替换资源: installed in-memory HTML replacement.");
}

std::filesystem::path EnsureReplacementFile() {
    const auto path = ExeDir() / L"vip-resource-replacement.html";
    if (!std::filesystem::exists(path)) {
        std::ofstream out(path, std::ios::binary);
        out << "<html><head><meta charset=\"UTF-8\"><title>VIP File Replace</title></head>"
            << "<body><h2>Local file replacement</h2><p>VIP高级功能测试 local file.</p></body></html>";
    }
    return path;
}

void ResourceFile() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"用本地文件替换资源", &vip)) return;
    const auto path = EnsureReplacementFile();
    FBroHsVIPControl_AddResourceHandlerChangeFile(vip, 0,
        CefString("https://www.baidu.com/"), CefString("text/html"),
        nullptr, CefString(path.wstring()));
    ReloadPage();
    AppendLogDirect(L"用本地文件替换资源: " + path.wstring());
}

void ResourceClear() {
    CefRefPtr<FBroVIPControl> vip;
    if (!EnsureVip(L"取消替换资源", &vip)) return;
    FBroHsVIPControl_DeleteResourceHandlerChangeData(vip,
        CefString("https://www.baidu.com/"));
    ReloadPage();
    AppendLogDirect(L"取消替换资源: cleared ResourceHandler replacement.");
}

void DomGetDocument() {
    if (!g_browser) {
        AppendLogDirect(L"枚举DOM_同步: browser is null.");
        return;
    }
    FBroHsDevToolsDOM_enable(g_browser, CefString(""));
    FBroHsDevToolsDOM_getDocument(g_browser, -1, TRUE,
        g_general_callback, nullptr, 0);
    AppendLogDirect(L"枚举DOM_同步: requested DOM.getDocument.");
}

void DomSearch() {
    if (!g_browser) {
        AppendLogDirect(L"查找文本_同步: browser is null.");
        return;
    }
    FBroHsDevToolsDOM_performSearch(g_browser, CefString("百度一下"), TRUE,
        g_general_callback, nullptr, 0);
    AppendLogDirect(L"查找文本_同步: requested DOM.performSearch.");
}

int TargetNode() {
    if (g_target_node_id != 0) return g_target_node_id;
    return g_root_node_id;
}

void DomQuerySelector() {
    if (!g_browser) {
        AppendLogDirect(L"查询节点选择器_同步: browser is null.");
        return;
    }
    if (g_root_node_id == 0) {
        DomGetDocument();
        AppendLogDirect(L"查询节点选择器_同步: root node unknown, requested document first.");
        return;
    }
    FBroHsDevToolsDOM_querySelector(g_browser, g_root_node_id,
        CefString("input.s_ipt"), g_general_callback, nullptr, 0);
    AppendLogDirect(L"查询节点选择器_同步: requested input.s_ipt.");
}

void DomQuerySelectorAll() {
    if (!g_browser) {
        AppendLogDirect(L"查询全部节点选择器_同步: browser is null.");
        return;
    }
    if (g_root_node_id == 0) {
        DomGetDocument();
        AppendLogDirect(L"查询全部节点选择器_同步: root node unknown, requested document first.");
        return;
    }
    FBroHsDevToolsDOM_querySelectorAll(g_browser, g_root_node_id,
        CefString("input"), g_general_callback, nullptr, 0);
    AppendLogDirect(L"查询全部节点选择器_同步: requested input.");
}

void DomFillForm() {
    if (!g_browser) {
        AppendLogDirect(L"模拟DOM填表: browser is null.");
        return;
    }
    if (g_target_node_id == 0) {
        DomQuerySelector();
        AppendLogDirect(L"模拟DOM填表: target node unknown, requested input node first.");
        return;
    }
    FBroHsDevToolsDOM_focusElement(g_browser, g_target_node_id);
    FBroHsDevToolsDOM_setAttributeValue(g_browser, g_target_node_id,
        CefString("value"), CefString("DOM填表测试" + std::to_string(g_target_node_id)));
    AppendLogDirect(L"模拟DOM填表: set input value on node " +
        std::to_wstring(g_target_node_id));
}

void HandleDomButton(int id) {
    if (!g_browser) {
        AppendLogDirect(L"DOM: browser is null.");
        return;
    }
    const int node = TargetNode();
    switch (id) {
    case kBtnDomDocument:
        DomGetDocument();
        break;
    case kBtnDomSearch:
        DomSearch();
        break;
    case kBtnDomRemoveAttribute:
        if (node) {
            FBroHsDevToolsDOM_removeAttribute(g_browser, node, CefString("content"));
            AppendLogDirect(L"移除节点属性: content, node=" + std::to_wstring(node));
        } else {
            AppendLogDirect(L"移除节点属性: no node id. Click 枚举DOM_同步 first.");
        }
        break;
    case kBtnDomRemoveNode:
        if (g_target_node_id) {
            FBroHsDevToolsDOM_removeNode(g_browser, g_target_node_id);
            AppendLogDirect(L"移除节点: node=" + std::to_wstring(g_target_node_id));
        } else {
            AppendLogDirect(L"移除节点: target node unknown. Click 查询节点选择器_同步 first.");
        }
        break;
    case kBtnDomSetAttributeText:
        if (node) {
            FBroHsDevToolsDOM_setAttributesAsText(g_browser, node,
                CefString("value=\"这是个插入的属性文本value\""), CefString("value"));
            AppendLogDirect(L"置节点属性文本: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomSetAttributeValue:
        if (node) {
            FBroHsDevToolsDOM_setAttributeValue(g_browser, node,
                CefString("value"), CefString("这是个插入的属性value"));
            AppendLogDirect(L"置节点属性值: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomSetNodeValue:
        if (node) {
            FBroHsDevToolsDOM_setNodeValue(g_browser, node, CefString("这是个节点值"));
            AppendLogDirect(L"置节点值: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomSetOuterHtml:
        if (node) {
            FBroHsDevToolsDOM_setOuterHTML(g_browser, node,
                CefString("<div id=\"native-vip-dom\">这是个源码</div>"));
            AppendLogDirect(L"置节点源码: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomGetAttribute:
        if (node) {
            FBroHsDevToolsDOM_getAttributes(g_browser, node,
                g_general_callback, nullptr, 0);
            AppendLogDirect(L"取节点属性值_同步: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomGetOuterHtml:
        if (node) {
            FBroHsDevToolsDOM_getOuterHTML(g_browser, node,
                g_general_callback, nullptr, 0);
            AppendLogDirect(L"取节点源码_同步: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomQuerySelector:
        DomQuerySelector();
        break;
    case kBtnDomQuerySelectorAll:
        DomQuerySelectorAll();
        break;
    case kBtnDomSetNodeName:
        if (node) {
            FBroHsDevToolsDOM_setNodeName(g_browser, node, CefString("fbrowser"),
                g_general_callback, nullptr, 0);
            AppendLogDirect(L"置节点名_同步: node=" + std::to_wstring(node));
        }
        break;
    case kBtnDomFillForm:
        DomFillForm();
        break;
    default:
        break;
    }
}

void HandleButton(int id) {
    switch (id) {
    case kBtnSendMessage:
        SendDevToolsMessage();
        break;
    case kBtnExecuteMethod:
        ExecuteDevToolsMethod();
        break;
    case kBtnEnableObserver:
        EnableDevToolsObserver();
        break;
    case kBtnDisableObserver:
        DisableDevToolsObserver();
        break;
    case kBtnScreenshot:
        CaptureScreenshot();
        break;
    case kBtnTouchEvent:
        DispatchTouch(0, 100, 100);
        DispatchTouch(1, 60, 60);
        DispatchTouch(3, 0, 0);
        AppendLogDirect(L"VIP触摸事件: dispatched start/move/end.");
        break;
    case kBtnTouchSwipe:
        DispatchTouch(0, 0, 100);
        DispatchTouch(1, 0, 50);
        DispatchTouch(3, 0, 0);
        AppendLogDirect(L"触摸测试_滑动: dispatched.");
        break;
    case kBtnTouchClick:
        DispatchTouch(0, 32, 32);
        DispatchTouch(3, 32, 32);
        AppendLogDirect(L"触摸测试_点击: dispatched.");
        break;
    case kBtnKeyCtrlA:
        DispatchMouse(CefString("mousePressed"), 100, 100, 0, CefString("left"), 1, 1, 0, 0);
        DispatchMouse(CefString("mouseReleased"), 100, 100, 0, CefString("left"), 0, 1, 0, 0);
        DispatchKey(CefString("keyDown"), 2, CefString(""), 'A');
        DispatchKey(CefString("keyUp"), 2, CefString(""), 'A');
        AppendLogDirect(L"按键测试_C+A: dispatched Ctrl+A.");
        break;
    case kBtnKeyText: {
        const std::wstring text = L"你好abcdefghijklmnopqrstuvwxyz1234567890-=~!@#$%^&*()！\n\r";
        for (wchar_t ch : text) {
            DispatchKey(CefString("char"), 0, CefString(std::wstring(1, ch)), 0);
        }
        AppendLogDirect(L"按键测试_输入文本: dispatched characters.");
        break;
    }
    case kBtnMouseClick:
        DispatchMouse(CefString("mouseWheel"), 0, 0, 0, CefString(""), 0, 0, -1000, 0);
        DispatchMouse(CefString("mouseMoved"), 32, 40, 0, CefString(""), 0, 0, 0, 0);
        DispatchMouse(CefString("mousePressed"), 32, 40, 0, CefString("left"), 1, 1, 0, 0);
        DispatchMouse(CefString("mouseReleased"), 32, 40, 0, CefString("left"), 0, 1, 0, 0);
        AppendLogDirect(L"鼠标测试_移动点击: dispatched.");
        break;
    case kBtnMouseWheel:
        DispatchMouse(CefString("mouseWheel"), 0, 0, 0, CefString(""), 0, 0, 1000, 1000);
        AppendLogDirect(L"鼠标测试_滚轮: dispatched.");
        break;
    case kBtnRuntimeEnable: {
        CefRefPtr<FBroVIPControl> vip;
        if (EnsureVip(L"VIP启用环境", &vip)) {
            FBroHsVIPControl_RuntimeEnable(vip, TRUE);
            AppendLogDirect(L"VIP启用环境: Runtime enabled.");
        }
        break;
    }
    case kBtnRuntimeDisable: {
        CefRefPtr<FBroVIPControl> vip;
        if (EnsureVip(L"VIP关闭环境", &vip)) {
            FBroHsVIPControl_RuntimeEnable(vip, FALSE);
            AppendLogDirect(L"VIP关闭环境: Runtime disabled.");
        }
        break;
    }
    case kBtnContextList:
        ContextList();
        break;
    case kBtnRuntimeJs:
        RuntimeEval(CefString("alert('Hello VIP');function text(a){return a;};text('21313你好')"),
            g_last_context_id, -1, 0, CefString(""), L"VIP执行JS");
        break;
    case kBtnRuntimeFrameId:
        RuntimeEval(CefString("function test(){return 'frameId:' + location.href;};test()"),
            0, 0, 0, CefString(g_last_frame_id), L"VIP执行JS_框架ID");
        break;
    case kBtnRuntimeMainFrame:
        RuntimeEval(CefString("function test(){return 1.23;};test()"),
            0, 1, 0, CefString(""), L"VIP执行JS_主框架");
        break;
    case kBtnRuntimeAllFrames:
        RuntimeEval(CefString("console.log('VIP all frames test')"),
            0, 2, 0, CefString(""), L"VIP执行JS_全部框架");
        break;
    case kBtnRuntimeFrameIndex:
        RuntimeEval(CefString("function test(){return location.href;};test()"),
            0, 3, 0, CefString(""), L"VIP执行JS_框架序号");
        break;
    case kBtnFilterChange:
        FilterChange();
        break;
    case kBtnFilterClear:
        FilterClear();
        break;
    case kBtnResourceData:
        ResourceData();
        break;
    case kBtnResourceFile:
        ResourceFile();
        break;
    case kBtnResourceClear:
        ResourceClear();
        break;
    default:
        HandleDomButton(id);
        break;
    }
}

HWND CreateButton(HWND parent, int id, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return hwnd;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_general_callback = new GeneralCallback();
        g_buttons.reserve(std::size(kButtons));
        for (const auto& spec : kButtons) {
            g_buttons.push_back(CreateButton(hwnd, spec.id, spec.text));
        }
        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_log_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_log_box, WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        AppendLogDirect(L"VIP高级功能测试 uses local .env for license.");
        LayoutChildren();
        return 0;
    case WM_COMMAND:
        if (IsKnownButtonId(LOWORD(wparam))) {
            HandleButton(LOWORD(wparam));
        }
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
            AppendLogDirect(message->text);
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
        L"VIP高级功能测试",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1500, 920,
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
    if (FBroGetProcessType() != BrowserProcess) {
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"VIPAdvancedFeatureDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"VIPAdvancedFeatureDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    g_license_ready = ApplyVipLicenseFromEnv();
    AppendLogDirect(g_license_ready ?
        L"VIP license loaded from local .env." :
        L"VIP license not confirmed. Advanced VIP APIs may fail.");

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"VIPAdvancedFeatureDemo", MB_ICONERROR);
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
    g_devtools_observer = nullptr;
    g_general_callback = nullptr;
    g_init_event = nullptr;
    WSACleanup();
    if (SUCCEEDED(co_result)) CoUninitialize();
    return 0;
}
