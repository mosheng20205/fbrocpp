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
#include "FBroSocket.h"
#include "FBroString.h"
#include "FBroVIPStruct.h"
#include "FBroVIPControl.h"
#include "FBroVIPInterface.h"
#include "FBroWSSClient.h"
#include "include/base/cef_bind.h"
#include "include/wrapper/cef_closure_task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"VIPWebsocketInterceptDemo.MainWindow";
constexpr char kCommandChangeUrl[] = "websocketchangeurl";
constexpr char kCommandChangeText[] = "websocketchangetext";
constexpr char kCommandChangeData[] = "websocketchangedata";
constexpr char kCommandSendText[] = "websocketsendtext";
constexpr char kCommandSendData[] = "websocketsenddata";
constexpr char kCommandClear[] = "websocketclear";
constexpr char kCommandProbe[] = "websocketprobe";
constexpr char kRenderLogName[] = "websocket";
constexpr char kWebSocketPageUrl[] = "http://www.websocket-test.com/";
constexpr int kWebSocketTextFrameType = 0;

constexpr int kButtonEnableHook = 1001;
constexpr int kButtonChangeUrl = 1002;
constexpr int kButtonChangeText = 1003;
constexpr int kButtonChangeBytes = 1004;
constexpr int kButtonSendText = 1005;
constexpr int kButtonSendBytes = 1006;
constexpr int kButtonClear = 1007;
constexpr int kLeftPanelWidth = 330;
constexpr UINT kMsgMaybeDestroy = WM_APP + 301;
constexpr UINT kMsgAppendLog = WM_APP + 302;
constexpr UINT_PTR kCloseFallbackTimer = 401;
constexpr UINT_PTR kRendererLogPollTimer = 402;
constexpr UINT kCloseFallbackMs = 5000;
constexpr UINT kRendererLogPollMs = 500;

HWND g_main_window = nullptr;
HWND g_browser_host = nullptr;
HWND g_url_label = nullptr;
HWND g_url_edit = nullptr;
HWND g_change_text_label = nullptr;
HWND g_change_text_edit = nullptr;
HWND g_send_text_label = nullptr;
HWND g_send_text_edit = nullptr;
HWND g_bytes_label = nullptr;
HWND g_bytes_edit = nullptr;
HWND g_log_box = nullptr;
HWND g_buttons[7]{};
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<FBroHsInitEvent> g_init_event;
bool g_fbro_ready = false;
bool g_license_ready = false;
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;
bool g_websocket_hook_enabled = false;
uintmax_t g_renderer_log_offset = 0;

// These globals live separately in the renderer process and are controlled by
// FBro main-process messages, matching the Volcano VIP WebSocket sample.
CefRefPtr<FBroDOMWssClient> g_render_websocket;
std::string g_render_change_url;
std::vector<char> g_render_change_text;
std::vector<char> g_render_change_data;
std::mutex g_returned_buffers_mutex;
std::unordered_set<void*> g_returned_buffers;

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
    if (size <= 0) {
        const int ansi_size = MultiByteToWideChar(CP_ACP, 0, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (ansi_size <= 0) return {};
        std::wstring result(ansi_size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, value.data(),
            static_cast<int>(value.size()), result.data(), ansi_size);
        return result;
    }

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
    std::ofstream out(ExeDir() / L"vip-websocket-intercept-demo.log",
        std::ios::app | std::ios::binary);
    out << ToUtf8(message) << "\n";
}

void RendererLogLine(const std::wstring& message) {
    OutputDebugStringW((message + L"\n").c_str());
    std::ofstream out(ExeDir() / L"vip-websocket-renderer.log",
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

std::wstring ReadWindowText(HWND hwnd) {
    if (!hwnd) return {};
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

std::string ReadWindowTextUtf8(HWND hwnd) {
    return ToUtf8(ReadWindowText(hwnd));
}

bool IsHexToken(const std::wstring& token) {
    if (token.empty() || token.size() > 2) return false;
    return std::all_of(token.begin(), token.end(), [](wchar_t ch) {
        return std::iswxdigit(ch) != 0;
    });
}

int HexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return 0;
}

std::vector<char> ParseBytesEdit() {
    const std::wstring text = ReadWindowText(g_bytes_edit);
    std::wistringstream stream(text);
    std::wstring token;
    std::vector<char> bytes;
    bool saw_token = false;
    bool all_hex = true;

    while (stream >> token) {
        saw_token = true;
        if (token.rfind(L"0x", 0) == 0 || token.rfind(L"0X", 0) == 0) {
            token = token.substr(2);
        }
        if (!IsHexToken(token)) {
            all_hex = false;
            break;
        }
        int value = 0;
        for (wchar_t ch : token) {
            value = value * 16 + HexValue(ch);
        }
        bytes.push_back(static_cast<char>(value & 0xFF));
    }

    if (saw_token && all_hex) {
        return bytes;
    }

    const std::string utf8 = ToUtf8(text);
    return std::vector<char>(utf8.begin(), utf8.end());
}

std::wstring HexPreview(const char* data, int size) {
    if (!data || size <= 0) return L"(empty)";
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    const int count = MinValue(size, 32);
    std::wstring result;
    for (int i = 0; i < count; ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        if (i > 0) result += L' ';
        result += kHex[(ch >> 4) & 0x0F];
        result += kHex[ch & 0x0F];
    }
    if (size > count) result += L" ...";
    return result;
}

std::wstring PayloadPreview(const char* data, int size) {
    if (!data || size <= 0) return L"size=0";
    const std::string bytes(data, data + MinValue(size, 160));
    std::wstring text = FromUtf8(bytes);
    for (wchar_t& ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    if (text.size() > 80) {
        text = text.substr(0, 80) + L"...";
    }

    std::wstringstream stream;
    stream << L"size=" << size << L", text=\"" << text
           << L"\", hex=" << HexPreview(data, size);
    return stream.str();
}

char* CopyToReturnedBuffer(const std::string& text) {
    if (text.empty()) return nullptr;
    char* buffer = new char[text.size() + 1];
    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    {
        std::lock_guard<std::mutex> lock(g_returned_buffers_mutex);
        g_returned_buffers.insert(buffer);
    }
    return buffer;
}

char* CopyToReturnedBuffer(const std::vector<char>& bytes) {
    if (bytes.empty()) return nullptr;
    char* buffer = new char[bytes.size()];
    memcpy(buffer, bytes.data(), bytes.size());
    {
        std::lock_guard<std::mutex> lock(g_returned_buffers_mutex);
        g_returned_buffers.insert(buffer);
    }
    return buffer;
}

void FreeReturnedBufferIfOwned(HANDLE data) {
    if (!data) return;
    {
        std::lock_guard<std::mutex> lock(g_returned_buffers_mutex);
        const auto it = g_returned_buffers.find(data);
        if (it == g_returned_buffers.end()) {
            return;
        }
        g_returned_buffers.erase(it);
    }
    delete[] static_cast<char*>(data);
}

void SendRendererLog(CefRefPtr<CefBrowser> browser, const std::wstring& text) {
    LogLine(L"[renderer] " + text);
    PostLog(L"renderer: " + text);
    if (!browser) return;
    const std::string utf8 = ToUtf8(text);
    FBroHsSocketClient_SendByBrowser(browser, kRenderLogName,
        utf8.empty() ? "" : utf8.data(), static_cast<int>(utf8.size()));
}

void SendRendererLogDeferred(CefRefPtr<CefBrowser> browser, std::wstring text) {
    RendererLogLine(L"renderer: " + text);
    (void)browser;
}

void PollRendererLogFile() {
    const auto path = ExeDir() / L"vip-websocket-renderer.log";
    if (!std::filesystem::exists(path)) return;

    const uintmax_t size = std::filesystem::file_size(path);
    if (size < g_renderer_log_offset) {
        g_renderer_log_offset = 0;
    }
    if (size <= g_renderer_log_offset) return;

    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(g_renderer_log_offset), std::ios::beg);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (!line.empty()) {
            AppendLogDirect(FromUtf8(line));
        }
    }
    g_renderer_log_offset = size;
}

bool HasLiveResources() {
    return g_browser != nullptr;
}

bool IsCefSubprocessCommandLine() {
    const wchar_t* command_line = GetCommandLineW();
    return command_line && wcsstr(command_line, L"--type=") != nullptr;
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

void LayoutChildren() {
    if (!g_main_window) return;

    RECT rc{};
    GetClientRect(g_main_window, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = 14;
    int y = 12;
    const int edit_width = kLeftPanelWidth - 28;

    auto move = [](HWND hwnd, int px, int py, int w, int h) {
        if (hwnd) MoveWindow(hwnd, px, py, MaxValue(1, w), MaxValue(1, h), TRUE);
    };

    move(g_url_label, x, y, edit_width, 18);
    y += 20;
    move(g_url_edit, x, y, edit_width, 26);
    y += 34;
    move(g_change_text_label, x, y, edit_width, 18);
    y += 20;
    move(g_change_text_edit, x, y, edit_width, 26);
    y += 34;
    move(g_send_text_label, x, y, edit_width, 18);
    y += 20;
    move(g_send_text_edit, x, y, edit_width, 26);
    y += 34;
    move(g_bytes_label, x, y, edit_width, 18);
    y += 20;
    move(g_bytes_edit, x, y, edit_width, 26);
    y += 42;

    const int button_width = 144;
    const int button_height = 30;
    for (int i = 0; i < 7; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        move(g_buttons[i], x + col * (button_width + 8),
            y + row * (button_height + 8), button_width, button_height);
    }
    y += 4 * (button_height + 8) + 6;
    move(g_log_box, x, y, edit_width, MaxValue(90, height - y - 12));

    move(g_browser_host, kLeftPanelWidth, 0,
        MaxValue(1, width - kLeftPanelWidth), height);

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

void SendRendererCommand(const char* command, const std::string& payload) {
    if (!g_browser) {
        AppendLogDirect(L"browser is not ready.");
        return;
    }

    const int ok = FBroHsSocketServer_SendByBrowser(g_browser, command,
        payload.empty() ? "" : payload.data(), static_cast<int>(payload.size()));
    AppendLogDirect(L"main -> renderer: " + FromUtf8(command) +
        L", bytes=" + std::to_wstring(payload.size()) +
        L", ok=" + std::to_wstring(ok));
}

void SendRendererCommand(const char* command, const std::vector<char>& payload) {
    SendRendererCommand(command, std::string(payload.begin(), payload.end()));
}

void EnableVipWebSocketHook() {
    if (!g_browser) {
        AppendLogDirect(L"browser is not ready.");
        return;
    }
    if (!g_license_ready) {
        AppendLogDirect(L"VIP license was not confirmed. Check local .env.");
    }

    CefRefPtr<FBroVIPControl> vip = FBroHsBrowser_GetVIPControl(g_browser);
    if (!vip || FBroHsVIPControl_IsNULL(vip)) {
        AppendLogDirect(L"FBro VIP control is null; websocket hook was not enabled.");
        return;
    }

    if (!g_websocket_hook_enabled) {
        FBroHsVIPControl_EnableWebsocketClientHook(vip);
        g_websocket_hook_enabled = true;
        AppendLogDirect(L"FBro VIP websocket hook enabled. Reloading test page.");
    } else {
        AppendLogDirect(L"FBro VIP websocket hook already enabled. Reloading test page.");
    }
    SendRendererCommand(kCommandProbe, std::string("after enable hook"));
    FBroHsBrowser_Reload(g_browser);
}

void HandleButton(int id) {
    switch (id) {
    case kButtonEnableHook:
        EnableVipWebSocketHook();
        break;
    case kButtonChangeUrl:
        SendRendererCommand(kCommandChangeUrl, ReadWindowTextUtf8(g_url_edit));
        break;
    case kButtonChangeText:
        SendRendererCommand(kCommandChangeText, ReadWindowTextUtf8(g_change_text_edit));
        break;
    case kButtonChangeBytes:
        SendRendererCommand(kCommandChangeData, ParseBytesEdit());
        break;
    case kButtonSendText:
        SendRendererCommand(kCommandSendText, ReadWindowTextUtf8(g_send_text_edit));
        break;
    case kButtonSendBytes:
        SendRendererCommand(kCommandSendData, ParseBytesEdit());
        break;
    case kButtonClear:
        SendRendererCommand(kCommandClear, std::string());
        break;
    default:
        break;
    }
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
        PostLog(L"embedded browser created: " + FromUtf8(kWebSocketPageUrl));
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

    void ReceiveRenderProcessMessage(CefRefPtr<CefBrowser>,
                                     int64_t processid,
                                     const CefString& name,
                                     char* message,
                                     int size) override {
        const int safe_size = MaxValue(0, size);
        const std::string bytes =
            (message && safe_size > 0) ? std::string(message, message + safe_size) : std::string();
        std::wstring text = FromUtf8(bytes);
        if (name.ToString() == kRenderLogName) {
            PostLog(L"renderer[" + std::to_wstring(processid) + L"]: " + text);
        } else {
            PostLog(L"renderer message " + name.ToWString() + L": " + text);
        }
    }

private:
    IMPLEMENT_REFCOUNTING(BrowserEvent);
};

bool IsRenderWebSocketValid() {
    return g_render_websocket != nullptr;
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

    void OnContextInitialized() override;

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        FBroHsCommandLine_DisableGpuBlockList(command_line);
    }

    void ReceiveMainProcessMessage(CefRefPtr<CefBrowser> browser,
                                   const CefString& name,
                                   char* message,
                                   int size) override {
        const std::string command = name.ToString();
        const int safe_size = MaxValue(0, size);
        const std::string payload =
            (message && safe_size > 0) ? std::string(message, message + safe_size) : std::string();

        if (command == kCommandChangeUrl) {
            g_render_change_url = payload;
            SendRendererLogDeferred(browser, L"changed websocket url to: " + FromUtf8(payload));
        } else if (command == kCommandChangeText) {
            g_render_change_text.assign(payload.begin(), payload.end());
            g_render_change_data.clear();
            SendRendererLogDeferred(browser, L"changed outgoing text payload: " + FromUtf8(payload));
        } else if (command == kCommandChangeData) {
            g_render_change_data.assign(payload.begin(), payload.end());
            g_render_change_text.clear();
            SendRendererLogDeferred(browser, L"changed outgoing byte payload: " +
                PayloadPreview(payload.data(), static_cast<int>(payload.size())));
        } else if (command == kCommandSendText) {
            if (!IsRenderWebSocketValid()) {
                SendRendererLogDeferred(browser, L"send text failed: no websocket client captured.");
                return;
            }
            FBroHsWSSClient_Send(g_render_websocket, CefString(FromUtf8(payload)));
            SendRendererLogDeferred(browser, L"sent websocket text: " + FromUtf8(payload));
        } else if (command == kCommandSendData) {
            if (!IsRenderWebSocketValid()) {
                SendRendererLogDeferred(browser, L"send bytes failed: no websocket client captured.");
                return;
            }
            if (payload.empty()) {
                SendRendererLogDeferred(browser, L"send bytes skipped: payload is empty.");
                return;
            }
            FBroHsWSSClient_SendData(g_render_websocket,
                const_cast<char*>(payload.data()), payload.size());
            SendRendererLogDeferred(browser, L"sent websocket bytes: " +
                PayloadPreview(payload.data(), static_cast<int>(payload.size())));
        } else if (command == kCommandClear) {
            g_render_change_url.clear();
            g_render_change_text.clear();
            g_render_change_data.clear();
            SendRendererLogDeferred(browser, L"cleared all websocket tamper data.");
        } else if (command == kCommandProbe) {
            SendRendererLogDeferred(browser, L"renderer process message channel is alive: " +
                FromUtf8(payload));
        }
    }

    void OnWebSocketClientCreate(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<FBroDOMWssClient> websocket) override {
        g_render_websocket = websocket;
        (void)browser;
        (void)frame;
        RendererLogLine(L"renderer: VIP websocket client created.");
    }

    void OnWebSocketClientClose(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefRefPtr<FBroDOMWssClient> websocket) override {
        if (g_render_websocket.get() == websocket.get()) {
            g_render_websocket = nullptr;
        }
        (void)frame;
        SendRendererLogDeferred(browser, L"VIP websocket client closed.");
    }

    void OnWebSocketClientConnect(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<FBroDOMWssClient>,
                                  HANDLE& returl,
                                  HANDLE& protocols) override {
        if (!g_render_change_url.empty()) {
            returl = CopyToReturnedBuffer(g_render_change_url);
        } else {
            returl = nullptr;
        }
        protocols = nullptr;

        (void)frame;
        SendRendererLogDeferred(browser, L"VIP websocket connect. changedUrl=" +
            (g_render_change_url.empty() ? L"(none)" : FromUtf8(g_render_change_url)));
    }

    bool OnWebSocketClientMessage(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<FBroDOMWssClient>,
                                  int type,
                                  HANDLE& data,
                                  int& size) override {
        (void)frame;
        std::vector<char> copy;
        const int safe_size = MaxValue(0, size);
        if (data && safe_size > 0) {
            const char* raw = static_cast<const char*>(data);
            copy.assign(raw, raw + safe_size);
        }
        SendRendererLogDeferred(browser, L"VIP websocket received, type=" +
            std::to_wstring(type) + L", " +
            PayloadPreview(copy.empty() ? nullptr : copy.data(),
                static_cast<int>(copy.size())));
        return false;
    }

    bool OnWebSocketClientSend(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               CefRefPtr<FBroDOMWssClient>,
                               int type,
                               HANDLE& retdata,
                               int,
                               int& size) override {
        (void)frame;
        std::vector<char> original;
        const int safe_size = MaxValue(0, size);
        if (retdata && safe_size > 0) {
            const char* raw = static_cast<const char*>(retdata);
            original.assign(raw, raw + safe_size);
        }

        SendRendererLogDeferred(browser, L"VIP websocket send, type=" +
            std::to_wstring(type) + L", original " +
            PayloadPreview(original.empty() ? nullptr : original.data(),
                static_cast<int>(original.size())));

        const bool is_text_frame = type == kWebSocketTextFrameType;

        if (!g_render_change_text.empty()) {
            if (!is_text_frame) {
                retdata = nullptr;
                SendRendererLogDeferred(browser,
                    L"skipped text tamper because this websocket frame is not text.");
                return false;
            }

            const std::string replacement(g_render_change_text.begin(),
                g_render_change_text.end());
            retdata = CopyToReturnedBuffer(replacement);
            // Volcano's text-to-bytes(TRUE) includes the trailing NUL in CVolMem.
            // Keep the same size contract for FBro's VIP hook wrapper.
            size = static_cast<int>(replacement.size() + 1);
            SendRendererLogDeferred(browser, L"outgoing websocket data replaced with text: " +
                FromUtf8(replacement));
        } else if (!g_render_change_data.empty()) {
            if (is_text_frame) {
                retdata = nullptr;
                SendRendererLogDeferred(browser,
                    L"skipped byte tamper for text frame; send a binary websocket frame to test byte replacement.");
                return false;
            }

            retdata = CopyToReturnedBuffer(g_render_change_data);
            size = static_cast<int>(g_render_change_data.size());
            SendRendererLogDeferred(browser, L"outgoing websocket data replaced with bytes: " +
                PayloadPreview(g_render_change_data.data(), size));
        } else {
            retdata = nullptr;
        }

        return false;
    }

    void ClearData(HANDLE data) override {
        FreeReturnedBufferIfOwned(data);
    }

private:
    IMPLEMENT_REFCOUNTING(InitEvent);
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
    extra->SetString("flag", "VIPWebsocketInterceptDemo");

    const BOOL ok = FBroHsCreate(CefString(kWebSocketPageUrl),
        &window_info, &browser_setting, nullptr, extra, event, nullptr,
        CefString("VIPWebsocketInterceptDemo"));
    if (!ok) {
        PostLog(L"FBroHsCreate failed.");
    }
}

void InitEvent::OnContextInitialized() {
    g_fbro_ready = true;
    PostLog(L"FBro context initialized.");
    CreateEmbeddedBrowser();
}

void SetControlFont(HWND hwnd) {
    if (!hwnd) return;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateLabel(HWND parent, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE,
        0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(hwnd);
    return hwnd;
}

HWND CreateEdit(HWND parent, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(hwnd);
    return hwnd;
}

HWND CreateButton(HWND parent, int id, const wchar_t* text) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SetControlFont(hwnd);
    return hwnd;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_url_label = CreateLabel(hwnd, L"WebSocket URL");
        g_url_edit = CreateEdit(hwnd, L"ws://127.0.0.1:8888");
        g_change_text_label = CreateLabel(hwnd, L"\u7be1\u6539\u6587\u672c\u6570\u636e");
        g_change_text_edit = CreateEdit(hwnd, L"FBro VIP changed text");
        g_send_text_label = CreateLabel(hwnd, L"\u53d1\u9001\u6587\u672c\u6570\u636e");
        g_send_text_edit = CreateEdit(hwnd, L"Hello from native C++");
        g_bytes_label = CreateLabel(hwnd, L"\u5b57\u8282\u96c6\u6570\u636e\uff08\u5341\u516d\u8fdb\u5236\u6216\u6587\u672c\uff09");
        g_bytes_edit = CreateEdit(hwnd, L"01 02 03 04");

        g_buttons[0] = CreateButton(hwnd, kButtonEnableHook,
            L"\u542f\u7528websocket\u62e6\u622a");
        g_buttons[1] = CreateButton(hwnd, kButtonChangeUrl,
            L"\u7be1\u6539\u94fe\u63a5");
        g_buttons[2] = CreateButton(hwnd, kButtonChangeText,
            L"\u7be1\u6539\u6587\u672c\u6570\u636e");
        g_buttons[3] = CreateButton(hwnd, kButtonChangeBytes,
            L"\u7be1\u6539\u5b57\u8282\u96c6\u6570\u636e");
        g_buttons[4] = CreateButton(hwnd, kButtonSendText,
            L"\u53d1\u9001\u6587\u672c\u6570\u636e");
        g_buttons[5] = CreateButton(hwnd, kButtonSendBytes,
            L"\u53d1\u9001\u5b57\u8282\u96c6\u6570\u636e");
        g_buttons[6] = CreateButton(hwnd, kButtonClear,
            L"\u6e05\u7a7a\u5168\u90e8\u7be1\u6539\u6570\u636e");

        g_log_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(g_log_box);

        g_browser_host = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        AppendLogDirect(L"Use FBro VIP websocket hook only. No JS hook path is used.");
        SetTimer(hwnd, kRendererLogPollTimer, kRendererLogPollMs, nullptr);
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
        if (wparam == kRendererLogPollTimer) {
            PollRendererLogFile();
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
        KillTimer(hwnd, kRendererLogPollTimer);
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
        L"VIPWebsocket\u62e6\u622a\u6d4b\u8bd5",
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
    // The VIP WebSocket callbacks run in the renderer process. Volcano sets the
    // subprocess path to the current executable so the renderer loads this same
    // InitEvent implementation instead of the generic FBroSubprocess.exe.
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
        MessageBoxW(nullptr, L"WSAStartup failed.", L"VIPWebsocketInterceptDemo", MB_ICONERROR);
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    if (!CreateMainWindow(instance, show_cmd)) {
        MessageBoxW(nullptr, L"Create main window failed.", L"VIPWebsocketInterceptDemo", MB_ICONERROR);
        WSACleanup();
        if (SUCCEEDED(co_result)) CoUninitialize();
        return 1;
    }

    std::error_code ignore_error;
    std::filesystem::remove(ExeDir() / L"vip-websocket-renderer.log", ignore_error);
    g_renderer_log_offset = 0;

    g_license_ready = ApplyVipLicenseFromEnv();
    AppendLogDirect(g_license_ready ?
        L"VIP license loaded from local .env." :
        L"VIP license not confirmed. The hook button may fail.");

    if (!InitFbro()) {
        MessageBoxW(nullptr, L"FBro initialization failed.", L"VIPWebsocketInterceptDemo", MB_ICONERROR);
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
