# FBro 接口盘点

本文档记录对当前项目依赖中的 `FBrowserCEF3lib.dll`、`FBrowserVIP.dll`、C++ 头文件、回调、窗口句柄和消息机制的初步盘点结果。

## 扫描范围

- DLL 运行时目录：`deps\CEFLib64`
- FBro C++ 头文件目录：`deps\FBrowserCEF3lib`
- 链接库目录：`deps\lib64`
- 火山 FBrowser 模块参考目录：`T:\编程工具\win_android\plugins\vprj_win\classlib\sys\FBrowser`

火山源码扫描时忽略了 `_int` 生成目录和 `*.~vbak.*` 备份文件。

## DLL 导出结论

`FBrowserCEF3lib.dll` 实际导出约 915 个函数，主要是基础浏览器和 CEF 封装。

`FBrowserVIP.dll` 实际导出约 180 个函数，主要是 VIP 指纹、DevTools、资源篡改、WebSocket 拦截、插件扩展等高级功能。

这些导出是 MSVC C++ 符号，名称经过 C++ name mangling，不是纯 C 风格 API。因此最佳做法是使用项目里的 `.h` 和 `.lib` 编译链接，不建议用 `GetProcAddress` 手写调用。

## 关键头文件

主要头文件位于 `deps\FBrowserCEF3lib`。

- `FBroInit.h`: 初始化、消息循环、关闭。
- `FBroBrowser.h`: 浏览器对象控制。
- `FBroBrowserHost.h`: 窗口句柄、输入、DevTools、截图、缩放。
- `FBroFrame.h`: Frame、JS 执行、DOM 访问。
- `FBroDom.h`: DOM 节点和文档操作。
- `FBroRequest.h`: 请求对象。
- `FBroResponse.h`: 响应对象。
- `FBroResourceHandler.h`: 自定义资源处理。
- `FBroResponseFilter.h`: 响应内容过滤和篡改。
- `FBroCookieManager.h`: Cookie 管理。
- `FBroV8Context.h`: V8 上下文。
- `FBroV8Value.h`: V8 值、对象、函数、Accessor、Interceptor。
- `FBroServer.h`: 内置 HTTP/WebSocket server。
- `FBroWebSocketClient.h`: WebSocket 客户端。
- `FBroWSSClient.h`: WSS 客户端。
- `FBroVIPInterface.h`: VIP 高级接口。
- `FBroVIPEvent.h`: VIP 事件。
- `FBroVIPEventInterface.h`: VIP 事件接口。
- `FBroTypes.h`: 大量结构体和回调 typedef。

## FBrowserCEF3lib 能力分类

`FBrowserCEF3lib.dll` 暴露的是基础浏览器能力和 CEF 封装能力。

- 浏览器生命周期：`FBroHsInitPro`、`FBroHsCreate`、`FBroHsCreateSync`、`FBroShutdown`、`FBroRunMessageLoop`、`FBroDoMessageLoopWork`、`FBroQuitMessageLoop`。
- 浏览器控制：前进、后退、刷新、忽略缓存刷新、停止加载、代理、缓存、Frame 获取、URL 加载。
- 窗口和句柄控制：`FBroHsBrowserHost_GetWindowHandle`、`FBroHsBrowserHost_GetParent`、`FBroHsBrowserHost_SetParent`、`FBroHsBrowserHost_MoveWindow`、`FBroHsBrowserHost_ShowWindows`、`FBroHsBrowserHost_SetWindowLong`、`FBroHsBrowserHost_GetWindowLong`。
- 输入模拟：鼠标点击、鼠标移动、滚轮、键盘、触摸、IME 输入、焦点。
- JS 和 DOM：执行 JS、带返回值执行 JS、访问 DOM、取源码、取文本、表单填充、点击、取属性、设属性。
- 网络拦截：Request、Response、ResourceHandler、ResponseFilter、Header、PostData。
- Cookie：读写、遍历、删除、Flush。
- V8 扩展：创建 JS 对象、函数、数组、V8 Handler、Accessor、Interceptor、Context Eval。
- 进程间消息：`FBroHsProcessMessage_*`、`SendProcessMessage`、主进程和渲染进程消息回调。
- 离屏渲染：`OnPaint`、`OnAcceleratedPaint`、`GetViewRect`、`GetScreenInfo`。
- 下载、文件对话框、PDF：下载回调、图片下载、文件选择、打印 PDF。
- 扩展和插件：加载扩展、扩展事件、扩展资源。
- 内置 Server/WebSocket：HTTP 请求、WebSocket 请求、连接、消息、关闭。

## FBrowserVIP 能力分类

`FBrowserVIP.dll` 暴露的是高级功能。

- 授权和机器码：获取机器码、版本、注册时间、到期时间、设置授权 Key。
- DevTools 协议：`Runtime.evaluate`、`Page.captureScreenshot`、`DOM.querySelector`、`DOM.getDocument`、`ExecuteDevToolsMethod`、`SendDevToolsMessage`。
- 指纹伪装：UserAgent、语言、平台、WebDriver、WebGL vendor/renderer、Canvas、Audio、Screen、Timezone、WebRTC IP、硬件并发、DeviceMemory、Battery、Viewport、Touch、插件等。
- 资源篡改：ResourceHandler/ResponseFilter 添加替换数据或替换文件。
- WebSocket 拦截：启用 WebSocket client hook，拦截 create、connect、send、message、close、error。
- 扩展增强：安装 CRX、加载扩展、获取扩展路径、URL、名称。
- 代理和认证：S5Auth、VIP 代理设置。

## 回调机制

回调类型主要集中在 `FBroTypes.h`，常见回调包括：

- 初始化和命令行：`OnContextInitialized_callback`、`OnBeforeCommandLineProcessing_callback`。
- 浏览器生命周期：`OnAfterCreated_callback`、`OnBeforeClose_callback`、`OnBrowserCreated_callback`、`OnBrowserDestroyed_callback`。
- 导航和加载：`OnBeforeBrowse_callback`、`OnLoadingStateChange_callback`、`OnLoadStart_callback`、`OnLoadEnd_callback`、`OnLoadError_callback`。
- 页面信息：`OnAddressChange_callback`、`OnTitleChange_callback`、`OnStatusMessage_callback`、`OnFaviconURLChange_callback`、`OnFullscreenModeChange_callback`。
- 弹窗和 DevTools：`OnBeforePopup_callback`、`OnBeforePopupAborted_callback`、`OnBeforeDevToolsPopup_callback`。
- 键盘、焦点、查找：`OnPreKeyEvent_callback`、`OnKeyEvent_callback`、`OnTakeFocus_callback`、`OnSetFocus_callback`、`OnGotFocus_callback`、`OnFindResult_callback`。
- 下载和文件对话框：`OnBeforeDownload_callback`、`OnDownloadUpdated_callback`、`OnFileDialog_callback`。
- 资源拦截：`GetResourceHandler_callback`、`GetResourceResponseFilter_callback`、`ResourceHandler_Open_callback`、`ResourceHandler_Read_callback`、`ResourceHandler_Cancel_callback`。
- 离屏渲染：`GetRootScreenRect_callback`、`GetViewRect_callback`、`GetScreenPoint_callback`、`GetScreenInfo_callback`、`OnPaint_callback`、`OnAcceleratedPaint_callback`。
- V8 和 JS：`V8HandlerExecute`、`ExecuteJavaScript_callback`、`OnContextCreated_callback`、`OnContextReleased_callback`、`OnUncaughtException_callback`。
- WebSocket：`Render_OnWebSocketCreate_callback`、`Render_OnWebSocketConnect_callback`、`Render_OnWebSocketSendText_callback`、`Render_OnWebSocketSendData_callback`、`Render_OnWebSocketMessage_callback`、`Render_OnWebSocketClose_callback`、`Render_OnWebSocketError_callback`。
- 进程消息：`Message_ReceiveRenderProcessMessage`、`Message_ReceiveMainProcessMessage`。
- URLRequest：`URLRequest_Start_callback`、`URLRequest_End_callback`、`URLRequest_OnRequestComplete_callback`、`URLRequest_OnUploadProgress_callback`、`URLRequest_OnDownloadProgress_callback`、`URLRequest_OnDownloadData_callback`。

## 窗口句柄机制

FBro 明确支持 Win32 `HWND`。

- `FBroBaseType.h` 中存在 `parent_window` 和 `window` 字段。
- `FBroBrowserHost.h` 中可以获取浏览器窗口句柄、获取父窗口、设置父窗口。
- CEF 原生 `CefWindowInfo::SetAsChild(parent, bounds)` 也存在。

因此 C++ 可以把浏览器嵌入 Win32、MFC、Qt 或自定义窗口。

常用入口：

- `FBroHsBrowserHost_GetWindowHandle`
- `FBroHsBrowserHost_GetOpenerWindowHandle`
- `FBroHsBrowserHost_GetParent`
- `FBroHsBrowserHost_SetParent`
- `FBroHsBrowserListControl_GetBrowserFromWindowHandle`

## 消息机制

消息机制主要有两层：

- CEF 进程消息：`CefProcessMessage`、`FBroHsProcessMessage_*`、`FBroHsBrowserFrame_SendProcessMessage`。
- FBro 封装的主进程和渲染进程回调：`Message_ReceiveRenderProcessMessage`、`Message_ReceiveMainProcessMessage`。

消息循环由以下函数控制：

- `FBroRunMessageLoop`
- `FBroDoMessageLoopWork`
- `FBroQuitMessageLoop`

## C++ 扩展建议

火山 demo 里的绝大多数能力，都已经在这些 C++ 头文件和两个 DLL 中暴露出来。后续扩展建议围绕以下文件做 C++ 封装：

- `FBroTypes.h`
- `FBroHsEvent.h`
- `FBroVIPInterface.h`
- `FBroVIPEvent.h`
- `FBroVIPEventInterface.h`

优先使用 `.h + .lib` 的静态链接入口，保持和当前 CMake 项目一致。只有在明确知道导出符号和调用约定时，才考虑动态加载。
