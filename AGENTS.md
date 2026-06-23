# Hstest Agent Notes

## FBrowser Volcano Package Layout

The Volcano FBrowser module installation log shows the package is extracted under:

`T:\编程工具\win_android\plugins\vprj_win\classlib\sys\FBrowser`

Important extracted directories:

- `src\env\FBrowserCEF3lib`
  - C/C++ header root.
  - Contains FBro headers such as `FBroBaseType.h`, `FBroBrowserHost.h`, `FBroFrame.h`, `FBroHsEvent.h`, `FBroInit.h`, `FBroCallback.h`, and CEF headers under `include\`.
- `src\env\lib`
  - 32-bit import libraries.
  - Has `debug` and `release` subdirectories.
- `src\env\lib64`
  - 64-bit import libraries.
  - Has `debug` and `release` subdirectories.
- `src\CEFLib`
  - 32-bit CEF runtime files.
- `src\CEFLib64`
  - 64-bit CEF runtime files.
  - Includes `locales`, `*.pak`, `icudtl.dat`, `snapshot_blob.bin`, `v8_context_snapshot.bin`, and related runtime resources.
- Module root `.v` files:
  - Volcano wrapper/module files such as `FBrowser.v`, `FBroVip.v`, `FBroLib.v`, `FBroCallback.v`, `FBroDataType.v`, `FBroConst.v`, and related files.

## Current C++ Dependency Copy

The current native C++ projects do not compile directly against the `T:\...` installation path.

They use a local copied dependency directory:

`C:\Users\Administrator\Downloads\Hstest\C++原生调用FBro\deps`

This local `deps` directory corresponds to the Volcano package pieces:

- Headers:
  - From `T:\...\FBrowser\src\env\FBrowserCEF3lib`
  - Used locally as `C++原生调用FBro\deps\FBrowserCEF3lib`
- 64-bit libs:
  - From `T:\...\FBrowser\src\env\lib64`
  - Used locally as `C++原生调用FBro\deps\lib64`
- 64-bit runtime:
  - From `T:\...\FBrowser\src\CEFLib64`
  - Used locally as `C++原生调用FBro\deps\CEFLib64`

## C++ Web Browser UI Project

Project directory:

`C:\Users\Administrator\Downloads\Hstest\C++网页浏览器UI`

Main source:

`C:\Users\Administrator\Downloads\Hstest\C++网页浏览器UI\src\main.cpp`

This project has no local `.h` or `.hpp` files of its own at the time of writing.

Its FBro/CEF include directory is configured in `CMakeLists.txt`:

`C:\Users\Administrator\Downloads\Hstest\C++原生调用FBro\deps\FBrowserCEF3lib`

The main source currently includes these FBro/CEF headers:

- `FBroBaseType.h`
- `FBroBrowserHost.h`
- `FBroControl.h`
- `FBroFrame.h`
- `FBroHsEvent.h`
- `FBroInit.h`
- `FBroCallback.h`
- `include/wrapper/cef_message_router.h`

The CMake link inputs include:

- `libcef_dll_wrapper.lib`
- `FBrowserCEF3lib.lib`
- `FBrowserVIP.lib`
- `libcef.lib`
- Windows system libraries such as `kernel32.lib`, `user32.lib`, `gdi32.lib`, `advapi32.lib`, `shell32.lib`, `ole32.lib`, `oleaut32.lib`, `ws2_32.lib`, `uuid.lib`, and `comctl32.lib`.

The project copies the runtime files from:

`C:\Users\Administrator\Downloads\Hstest\C++原生调用FBro\deps\CEFLib64`

to the executable output directory after build.

## Practical Rule

When updating or rebuilding native C++ FBro examples in this workspace:

1. Prefer the local `C++原生调用FBro\deps` dependency copy instead of directly referencing `T:\...`.
2. Use `deps\FBrowserCEF3lib` for headers.
3. Use `deps\lib64` for x64 import libraries.
4. Copy or stage `deps\CEFLib64` beside the built `.exe` for runtime.
5. If a missing header/lib/runtime file is discovered, compare against the original Volcano installation path from the module log.

## NativeFBroDemo Browser Creation Rules

Project directory:

`C:\Users\Administrator\Downloads\Hstest\C++原生调用FBro`

The demo intentionally supports two browser creation modes:

1. Embedded FBro browser
   - Create a real child control/window first, such as `g_embed_host`.
   - Pass that child control `HWND` as `E_WINDOWS_INFO.parent_window`.
   - Use `FBroHsCreate(...)`.
   - This mode demonstrates binding the browser to an existing component handle.

2. Native CEF/Chrome popup browser
   - Do not create an extra top-level Win32 host window manually.
   - Use native CEF creation directly:

```cpp
CefWindowInfo window_info;
window_info.SetAsPopup(nullptr, L"Native CEF Chrome Browser");
```

   - The parent handle must be `0` / `nullptr`.
   - This lets CEF create the independent popup window itself.

Verified behavior:

- `FBroHsCreate(...)` with a child component handle correctly creates the embedded browser.
- `CefBrowserHost::CreateBrowser(...)` with `SetAsPopup(nullptr, ...)` correctly creates the native popup browser.
- Creating a separate top-level Win32 host window and then binding native CEF into it is not the desired behavior for this demo.

## ServerWebDemo Crash Notes

Project directory:

`C:\Users\Administrator\Downloads\Hstest\FBroC++\服务器Web`

Main source:

`C:\Users\Administrator\Downloads\Hstest\FBroC++\服务器Web\src\main.cpp`

### Symptom

When the first `服务器Web` implementation was expanded to include:

- static file serving
- JSON API routes
- WebSocket echo

clicking `启动服务器Web` or sending a WebSocket message triggered Debug CRT heap assertions such as:

- `_CrtIsValidHeapPointer(block)`
- `is_block_type_valid(header->_block_use)`
- `HEAP CORRUPTION DETECTED: before/after Free block`

### What Was Verified Safe

The following pieces were verified incrementally and are safe on their own:

1. `FBroHsServer_CreateServer(...)` itself
2. The server event object shell:
   - `type_ = ServerHandleType`
   - custom `operator new/delete` using:
     - `FBroMallocManger_New`
     - `FBroMallocManger_Free`
3. `OnServerCreated`
4. `OnServerDestroyed`
5. `OnHttpRequest` returning a fixed static HTML buffer
6. `OnWebSocketRequest` with only log output plus `callback->Continue()`
7. `OnWebSocketConnected` with only log output
8. `OnWebSocketMessage` with only log output

### Root Cause Found During Recovery

The unstable part was not server creation itself. The crash point was the old pattern of echoing WebSocket data directly inside:

`OnWebSocketMessage(...)`

In other words, this pattern is unsafe in the current native C++ FBro server example:

- receive WebSocket message in callback
- immediately call `FBroHsServer_SendWebSocketMessage(...)` from the same callback context

That synchronous echo path caused heap corruption in Debug builds.

### Stable Fix

The stable implementation pattern is:

1. In `OnWebSocketMessage(...)`
   - do not send immediately
   - copy incoming message bytes into owned process memory
   - push `(connection_id, message)` into a queue
   - `PostMessage(...)` back to the main window

2. In the main window message handler
   - flush the queued WebSocket messages
   - call `FBroHsServer_SendWebSocketMessage(...)` from the UI/main thread

This async main-thread echo pattern was verified working:

- server starts normally
- embedded page loads normally
- WebSocket connects normally
- sending a test message returns echo successfully
- no Debug CRT heap corruption popup appears

### Practical Rules For Future FBro Server Examples

When building native C++ FBro server demos in this workspace:

1. Always set server event type explicitly:

```cpp
type_ = ServerHandleType;
```

2. For server event objects, keep the custom allocator pattern:

```cpp
static void* operator new(size_t size) {
    return FBroMallocManger_New(size);
}

static void operator delete(void* ptr) noexcept {
    if (ptr) {
        FBroMallocManger_Free(ptr);
    }
}
```

3. Treat callback-owned objects and callback timing conservatively.
4. Do not assume WebSocket send is safe when called synchronously inside the receive callback.
5. Prefer `PostMessage(...)` back to the main thread for follow-up actions such as:
   - browser navigation
   - UI updates
   - WebSocket echo/reply
6. If a server demo crashes, reduce it in this order:
   - server create only
   - add `OnHttpRequest`
   - add `OnWebSocketRequest`
   - add `OnWebSocketConnected`
   - add `OnWebSocketMessage` log only
   - add async echo last

### Stable ServerWebDemo Behavior

The current verified-stable `服务器Web` version supports:

- start local server on `127.0.0.1:7777`
- embedded browser loading the local page
- built-in page buttons for:
  - `Connect`
  - `Send Test Message`
- WebSocket echo via queued main-thread async send

## Native C++ FBro Shutdown Rules

These rules apply to every project in:

`C:\Users\Administrator\Downloads\Hstest\FBroC++\build\FBroCppSuite.sln`

Affected projects at the time of writing:

- `NativeFBroDemo`
- `BaiduFormFill`
- `Cookie设置取出`
- `服务器`
- `服务器Web`

### Volcano Reference Model

The Volcano close-event model is:

1. In the window close-question event, hide the window.
2. Call `FBrowser_关闭()` immediately.
3. Return `1` to block direct window closing.
4. Let the browser/framework callbacks finish resource release.

The Volcano wrapper confirms:

```cpp
FBrowser_关闭(结束程序 = 假) -> FBroShutdown(FALSE)
```

So the closest native C++ equivalent is not `FBroShutdown(TRUE)`. It is `FBroShutdown(FALSE)` during the close-request path.

### Unsafe Pattern

Avoid this direct shutdown pattern in native C++ examples:

```cpp
case WM_CLOSE:
    FBroHsBrowserHost_CloseBrowser(g_browser, true);
    DestroyWindow(hwnd);
    return 0;

// later after message loop
FBroShutdown(TRUE);
```

This was observed to leave the main process stuck or make `FBroSubprocess.exe` cleanup unreliable.

### Stable Pattern

Use a two-stage close model:

1. `WM_CLOSE`
   - set `g_close_requested = true`
   - `ShowWindow(hwnd, SW_HIDE)`
   - shut down owned server first if present:
     - `FBroHsServer_Shutdown(g_server)`
   - request browser close:
     - `FBroHsBrowserHost_CloseBrowser(g_browser, true)`
     - native CEF popup: `g_native_browser->GetHost()->CloseBrowser(true)`
   - call `FBroShutdown(FALSE)` once
   - do not repeatedly call shutdown

2. Browser/server callbacks
   - in `OnBeforeClose`, clear browser pointers
   - in `OnServerDestroyed`, clear server pointers and flags
   - use `PostMessage(...)` back to the main window for final destroy checks

3. Final destroy
   - when tracked browser/server resources are gone, call:
     - `DestroyWindow(g_main_window)`
     - `FBroQuitMessageLoop()`
     - `PostQuitMessage(0)`

4. Fallback
   - keep a short fallback timer, currently 5 seconds
   - if FBro does not deliver `OnBeforeClose` / server destroyed callback, clear local refs and continue process exit
   - this prevents hidden windows from hanging forever

5. After `FBroRunMessageLoop()` returns
   - only call `FBroShutdown(FALSE)` if shutdown was not already started
   - never use `FBroShutdown(TRUE)` for the normal user close path in these demos

### Verified Result

After applying the shutdown model above, all five solution projects were rebuilt and tested by launching each executable and sending a normal window close request.

Observed result:

- every main process exited
- `FBroSubprocess.exe` did not remain after close
- the close path may return a non-zero FBro/CEF internal exit code, but subprocess cleanup was verified clean

### Practical Rule For New Projects

When adding a new FBro C++ demo to this solution, copy the shutdown structure from the current projects instead of writing a simple `WM_CLOSE -> DestroyWindow`.

Minimum globals to carry:

```cpp
bool g_close_requested = false;
bool g_destroying_window = false;
bool g_fbro_shutdown_started = false;
```

Minimum helper idea:

```cpp
void RequestAppClose(HWND hwnd);
void MaybeDestroyAfterClose();
bool HasLiveResources();
```

For server projects, `HasLiveResources()` must include both browser and server state.

## SyncHelperDemo JS Return Rule

This applies to project:

`C:\Users\Administrator\Downloads\Hstest\FBroC++\同步辅助类`

### Volcano Generated C++ Clue

The Volcano generated project `同步执行JS测试` can call:

```cpp
FBroHsBrowserFrame_ExecuteJavaScriptToHasReturn(...)
```

and receive the result through a class shaped like:

```cpp
class rg_n8948 : public CVolObject, public FBroHsJsCallback
```

The callback result list uses this structure:

- `list[1]`: return type
- `list[2]`: normal return value
- `list[3]`: error string when type is `-1`

Observed type mapping:

- `1`: int
- `2`: string
- `3`: bool
- `4`: double
- `-1`: error

Volcano also wraps the wait state through `FBroSynEventDis_*`, then calls `SetEvent()` in the JS callback.

### Native C++ Abort Finding

In the pure native C++ project, reproducing the same shape with:

```cpp
class VolcanoStyleJsCallback final : public FBroHsJsCallback
```

plus:

- `type_ = JsCallbackType`
- `FBroMallocManger_New`
- `FBroMallocManger_Free`
- `FBroSynEventDis_CreatEvent`
- `FBroSynEventDis_WaitEvent`
- `FBroHsBrowserFrame_ExecuteJavaScriptToHasReturn`

still triggers a Microsoft Visual C++ Runtime Library Debug Error:

```text
abort() has been called
```

This confirms that the Volcano generated code is not only plain C++. The working Volcano path depends on its runtime object model, especially `CVolObject` / `DECLARE_VOL_CLASS` and related lifecycle hooks.

### Current Stable Implementation

Do not use `FBroHsBrowserFrame_ExecuteJavaScriptToHasReturn` directly in the native `同步辅助类` demo.

The `同步执行JS` button currently uses the stable fallback model:

1. call native CEF `frame->ExecuteJavaScript(...)`
2. write the JS return value into a temporary DOM marker
3. read it back with native CEF `frame->GetText(...)`
4. display the parsed result in the top-right edit box

This avoids the Debug CRT abort while still demonstrating a JS execution result in native C++.

### Practical Rule For Future Projects

For native C++ demos in this solution:

- use native CEF APIs for JS execution when a return value is needed
- keep `VolcanoStyleJsCallback` only as research/reference code unless the full Volcano object runtime is introduced
- do not wire `FBroHsBrowserFrame_ExecuteJavaScriptToHasReturn` to a hand-written C++ callback in user-facing buttons

## URLRequest Demo Rule

This applies to project:

`C:\Users\Administrator\Downloads\Hstest\FBroC++\创建URL请求`

### Volcano Reference

The Volcano `基础测试X64` generated code for `创建URL测试` uses:

```cpp
FBroHsURLRequest_Create(request, request_context, client, flag);
FBroHsBrowserFrame_CreateURLRequest(frame, request, client, flag);
```

The Volcano URL request event class is shaped like:

```cpp
class rg_n10780 : public CVolObject, public FBroHsURLRequestClient
```

and sets:

```cpp
type_ = URLRequestClientType;
```

The custom Volcano event logs:

- start
- download data
- progress
- complete/end

### Native C++ Abort Finding

Do not directly copy the Volcano URL request callback model into pure native C++.

The following native C++ attempts both triggered a Debug CRT heap assertion:

```text
Expression: _CrtIsValidHeapPointer(block)
```

Unsafe attempts:

```cpp
FBroHsURLRequest_Create(request, nullptr, hand_written_FBroHsURLRequestClient, 0);
```

and:

```cpp
frame->CreateURLRequest(request, native_CefURLRequestClient);
```

This is consistent with the JS callback finding: Volcano generated C++ is supported by `CVolObject` / `DECLARE_VOL_CLASS` runtime lifecycle code. A hand-written pure C++ callback object can cross an incompatible allocation/release boundary.

### Current Stable Implementation

Use native CEF URL request creation instead.

Global URL request:

```cpp
CefURLRequest::Create(request, client, nullptr);
```

Browser-context URL request, used by the button labeled `创建框架URL请求`:

```cpp
CefRefPtr<CefRequestContext> context =
    FBroHsBrowserHost_GetRequestContext(g_browser);
CefURLRequest::Create(request, client, context);
```

This preserves the practical distinction:

- global request: no explicit browser request context
- frame/browser request: uses the embedded browser's request context

but avoids the crashing frame URLRequest API.

### Response Body Rule

To show complete response content:

1. append each `OnDownloadData` chunk into a `std::string response_body_`
2. in `OnRequestComplete`, decode and print the full accumulated body

The current implementation decodes UTF-8 first, then falls back to the local ANSI code page.

### Verified Result

Both buttons have been rebuilt and tested:

- `创建全局URL请求`
- `创建框架URL请求`

Observed result:

```text
status=SUCCESS
url=https://api.fbrowser.site:8443/
http=200
mime=text/html
```

The process stayed responsive, and the full response body is displayed in the UI/log.
