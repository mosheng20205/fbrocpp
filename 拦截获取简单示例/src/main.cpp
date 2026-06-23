#include "pch.h"

#include "FBroBaseType.h"
#include "FBroBrowser.h"
#include "FBroBrowserHost.h"
#include "FBroControl.h"
#include "FBroFrame.h"
#include "FBroHsEvent.h"
#include "FBroInit.h"
#include "FBroRequest.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ResourceInterceptDemo.MainWindow";
constexpr int kToolbarHeight = 124;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT kMsgAppendLog = WM_APP + 302;
constexpr UINT kMsgLoadBaidu = WM_APP + 303;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT kCloseFallbackMs = 5000;

HWND g_main_window = nullptr;
HWND g_status_box = nullptr;
HWND g_browser_host = nullptr;
CefRefPtr<CefBrowser> g_browser;
bool g_fbro_ready = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;
std::atomic<unsigned long> g_capture_index{0};
CefRefPtr<CefRegistration> g_devtools_registration;
struct ResourceInfo {
    std::wstring url;
    std::wstring mime_type;
};
std::map<int, ResourceInfo> g_body_requests;
std::map<std::wstring, ResourceInfo> g_request_infos;

template <typename T>
T MinValue(T a, T b) {
    return a < b ? a : b;
}

template <typename T>
T MaxValue(T a, T b) {
    return a > b ? a : b;
}

struct LogMessage {
    std::wstring text;
};

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

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

std::wstring FromCefString(const CefString& value) {
    return value.ToWString();
}

std::wstring StatusName(CefResourceRequestHandler::URLRequestStatus status) {
    switch (status) {
    case CefResourceRequestHandler::URLRequestStatus::UR_SUCCESS:
        return L"SUCCESS";
    case CefResourceRequestHandler::URLRequestStatus::UR_IO_PENDING:
        return L"IO_PENDING";
    case CefResourceRequestHandler::URLRequestStatus::UR_CANCELED:
        return L"CANCELED";
    case CefResourceRequestHandler::URLRequestStatus::UR_FAILED:
        return L"FAILED";
    default:
        return L"UNKNOWN";
    }
}

std::wstring ResourceTypeName(cef_resource_type_t type) {
    switch (type) {
    case RT_MAIN_FRAME: return L"MAIN_FRAME";
    case RT_SUB_FRAME: return L"SUB_FRAME";
    case RT_STYLESHEET: return L"STYLESHEET";
    case RT_SCRIPT: return L"SCRIPT";
    case RT_IMAGE: return L"IMAGE";
    case RT_FONT_RESOURCE: return L"FONT";
    case RT_SUB_RESOURCE: return L"SUB_RESOURCE";
    case RT_XHR: return L"XHR";
    case RT_MEDIA: return L"MEDIA";
    case RT_FAVICON: return L"FAVICON";
    default: return L"OTHER";
    }
}

size_t MinSize(size_t a, size_t b) {
    return a < b ? a : b;
}

void LogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"resource-intercept-demo.log",
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

void AttachDevToolsNetworkObserver(CefRefPtr<CefBrowser> browser);

std::wstring SanitizeFileName(const std::wstring& value) {
    std::wstring result;
    result.reserve(MinValue<size_t>(value.size(), 120));
    for (wchar_t ch : value) {
        if (result.size() >= 120) break;
        if ((ch >= L'0' && ch <= L'9') ||
            (ch >= L'a' && ch <= L'z') ||
            (ch >= L'A' && ch <= L'Z') ||
            ch == L'.' || ch == L'-' || ch == L'_') {
            result.push_back(ch);
        } else {
            result.push_back(L'_');
        }
    }
    return result.empty() ? L"resource" : result;
}

std::wstring LowerAscii(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = ch - L'A' + L'a';
        }
    }
    return value;
}

std::wstring ExtensionFromUrl(const std::wstring& url) {
    std::wstring clean = url;
    const size_t hash_pos = clean.find(L'#');
    if (hash_pos != std::wstring::npos) clean.resize(hash_pos);
    const size_t query_pos = clean.find(L'?');
    if (query_pos != std::wstring::npos) clean.resize(query_pos);

    const size_t slash_pos = clean.find_last_of(L"/\\");
    const std::wstring last = slash_pos == std::wstring::npos
        ? clean
        : clean.substr(slash_pos + 1);
    const size_t dot_pos = last.find_last_of(L'.');
    if (dot_pos == std::wstring::npos || dot_pos + 1 >= last.size()) {
        return {};
    }

    std::wstring ext = LowerAscii(last.substr(dot_pos));
    if (ext.size() > 12) {
        return {};
    }
    return ext;
}

std::wstring ExtensionFromMimeOrUrl(const std::wstring& mime_type,
                                    const std::wstring& url) {
    const std::wstring mime = LowerAscii(mime_type);
    if (mime.find(L"text/html") != std::wstring::npos) return L".html";
    if (mime.find(L"text/css") != std::wstring::npos) return L".css";
    if (mime.find(L"javascript") != std::wstring::npos ||
        mime.find(L"ecmascript") != std::wstring::npos) return L".js";
    if (mime.find(L"json") != std::wstring::npos) return L".json";
    if (mime.find(L"image/png") != std::wstring::npos) return L".png";
    if (mime.find(L"image/jpeg") != std::wstring::npos ||
        mime.find(L"image/jpg") != std::wstring::npos) return L".jpg";
    if (mime.find(L"image/gif") != std::wstring::npos) return L".gif";
    if (mime.find(L"image/webp") != std::wstring::npos) return L".webp";
    if (mime.find(L"image/svg") != std::wstring::npos) return L".svg";
    if (mime.find(L"image/x-icon") != std::wstring::npos ||
        mime.find(L"image/vnd.microsoft.icon") != std::wstring::npos) return L".ico";
    if (mime.find(L"font/woff2") != std::wstring::npos) return L".woff2";
    if (mime.find(L"font/woff") != std::wstring::npos) return L".woff";
    if (mime.find(L"font/ttf") != std::wstring::npos) return L".ttf";

    const std::wstring url_ext = ExtensionFromUrl(url);
    return url_ext.empty() ? L".bin" : url_ext;
}

std::wstring TrimTrailingExtension(std::wstring file_name,
                                   const std::wstring& ext) {
    const std::wstring lower_name = LowerAscii(file_name);
    const std::wstring lower_ext = LowerAscii(ext);
    if (!lower_ext.empty() && lower_name.size() > lower_ext.size() &&
        lower_name.compare(lower_name.size() - lower_ext.size(),
            lower_ext.size(), lower_ext) == 0) {
        file_name.resize(file_name.size() - ext.size());
    }
    return file_name;
}

std::filesystem::path MakeCapturePath(const std::wstring& url,
                                      const std::wstring& mime_type) {
    const auto dir = ExeDir() / L"CapturedResources";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::wstring ext = ExtensionFromMimeOrUrl(mime_type, url);
    const std::wstring base_name = TrimTrailingExtension(SanitizeFileName(url), ext);

    std::wostringstream name;
    name << std::setw(4) << std::setfill(L'0') << ++g_capture_index
         << L"_" << base_name << ext;
    return dir / name.str();
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

    if (g_status_box) {
        MoveWindow(g_status_box, 12, 10, MaxValue(1, width - 24), 104, TRUE);
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
    if (!g_browser) {
        return;
    }

    CefRefPtr<CefFrame> frame = FBroHsBrowser_GetMainFrame(g_browser);
    if (!frame) {
        return;
    }
    FBroHsBrowserFrame_LoadURL(frame, CefString("https://www.baidu.com"));
    PostStatus(L"loaded https://www.baidu.com after DevTools observer attached.");
}

class BrowserEvent final : public FBroHsBroEvent {
public:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue>) override {
        g_browser = browser;
        PostStatus(L"browser created.");
        LayoutChildren();
        AttachDevToolsNetworkObserver(browser);
        if (g_main_window) {
            PostMessageW(g_main_window, kMsgLoadBaidu, 0, 0);
        }
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
            PostStatus(L"main page load end, status=" +
                std::to_wstring(http_status_code));
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

class DevToolsObserver final : public CefDevToolsMessageObserver {
public:
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>,
                                int message_id,
                                bool success,
                                const void* result,
                                size_t result_size) override {
        auto iter = g_body_requests.find(message_id);
        if (iter == g_body_requests.end()) {
            return;
        }

        const ResourceInfo info = iter->second;
        g_body_requests.erase(iter);

        if (!success || !result || result_size == 0) {
            PostStatus(L"body failed: " + info.url);
            return;
        }

        const std::string json(static_cast<const char*>(result), result_size);
        CefRefPtr<CefValue> parsed = CefParseJSON(json, JSON_PARSER_RFC);
        if (!parsed || !parsed->IsValid() || parsed->GetType() != VTYPE_DICTIONARY) {
            PostStatus(L"body parse failed: " + info.url);
            return;
        }

        CefRefPtr<CefDictionaryValue> root = parsed->GetDictionary();
        const std::wstring body = FromCefString(root->GetString("body"));
        const bool base64_encoded = root->GetBool("base64Encoded");
        std::string bytes;
        if (base64_encoded) {
            CefRefPtr<CefBinaryValue> decoded = CefBase64Decode(CefString(body));
            if (decoded) {
                bytes.resize(decoded->GetSize());
                decoded->GetData(bytes.data(), bytes.size(), 0);
            }
        } else {
            bytes = ToUtf8(body);
        }

        const auto path = MakeCapturePath(info.url, info.mime_type);
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();

        std::wostringstream line;
        line << L"devtools body bytes=" << bytes.size()
             << L", base64=" << (base64_encoded ? L"true" : L"false")
             << L", mime=" << info.mime_type
             << L", file=" << path.wstring()
             << L", url=" << info.url;
        PostStatus(line.str());
    }

    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser,
                         const CefString& method,
                         const void* params,
                         size_t params_size) override {
        if (!browser || !params || params_size == 0) {
            return;
        }

        const std::wstring method_name = FromCefString(method);
        const std::string json(static_cast<const char*>(params), params_size);
        CefRefPtr<CefValue> parsed = CefParseJSON(json, JSON_PARSER_RFC);
        if (!parsed || !parsed->IsValid() || parsed->GetType() != VTYPE_DICTIONARY) {
            return;
        }

        CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
        const std::wstring request_id = FromCefString(dict->GetString("requestId"));
        if (request_id.empty()) {
            return;
        }

        if (method_name == L"Network.responseReceived") {
            CefRefPtr<CefDictionaryValue> response = dict->GetDictionary("response");
            if (response) {
                const std::wstring url = FromCefString(response->GetString("url"));
                const std::wstring mime = FromCefString(response->GetString("mimeType"));
                g_request_infos[request_id] = ResourceInfo{url, mime};
                const int status = response->GetInt("status");
                std::wostringstream line;
                line << L"devtools response: http=" << status
                     << L", mime=" << mime
                     << L", requestId=" << request_id
                     << L", url=" << url;
                PostStatus(line.str());
            }
            return;
        }

        if (method_name == L"Network.loadingFinished") {
            CefRefPtr<CefDictionaryValue> params_dict = CefDictionaryValue::Create();
            params_dict->SetString("requestId", request_id);
            const int message_id = static_cast<int>(10000 + (++g_capture_index));
            auto info_iter = g_request_infos.find(request_id);
            g_body_requests[message_id] = info_iter == g_request_infos.end()
                ? ResourceInfo{request_id, L""}
                : info_iter->second;
            browser->GetHost()->ExecuteDevToolsMethod(
                message_id, CefString("Network.getResponseBody"), params_dict);
            return;
        }
    }

private:
    IMPLEMENT_REFCOUNTING(DevToolsObserver);
};

void AttachDevToolsNetworkObserver(CefRefPtr<CefBrowser> browser) {
    if (!browser || g_devtools_registration) {
        return;
    }

    CefRefPtr<DevToolsObserver> observer = new DevToolsObserver();
    g_devtools_registration =
        browser->GetHost()->AddDevToolsMessageObserver(observer);
    browser->GetHost()->ExecuteDevToolsMethod(
        1, CefString("Network.enable"), CefDictionaryValue::Create());
    PostStatus(L"DevTools Network observer attached.");
}

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
    extra->SetString("flag", "ResourceInterceptDemo");

    const BOOL ok = FBroHsCreate(CefString("about:blank"),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("ResourceInterceptDemo"));
    if (!ok) {
        PostStatus(L"FBroHsCreate failed.");
    }
}

class InitEvent final : public FBroHsInitEvent {
public:
    void OnContextInitialized() override {
        g_fbro_ready = true;
        PostStatus(L"FBro context initialized.");
        CreateEmbeddedBaiduBrowser();
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
        AppendStatusDirect(L"Resource interception is automatic. Captured files are saved beside the exe under CapturedResources.");
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
        L"\u62e6\u622a\u83b7\u53d6\u7b80\u5355\u793a\u4f8b",
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"ResourceInterceptDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"ResourceInterceptDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"ResourceInterceptDemo", MB_ICONERROR);
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
