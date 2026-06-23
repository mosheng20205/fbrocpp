# FBroC++ 示例工程

中文 | [English](README.en.md)

这是一个使用 C++ 调用 FBro / FBrowser CEF 模块的 Visual Studio / CMake 示例集合。项目来自火山 FBrowser 示例的 C++ 化整理，目标是把常用功能拆成独立、可运行、可学习的小项目。

## 项目截图

### 创建浏览器

![创建浏览器](image/CreateBrowser.png)

### 百度填表

![百度填表](image/FromFill.png)

### Cookie 设置取出

![Cookie 设置取出](image/Cookie.png)

## 当前项目

- `NativeFBroDemo`
  - 演示两种浏览器创建方式：
    - 内嵌 FBro 浏览器：绑定到指定组件 `HWND`
    - 原生 CEF/Chrome 弹窗：`SetAsPopup(nullptr, ...)`
  - 包含基础 VIP / VIPControl 调用测试代码。

- `BaiduFormFill`
  - 内嵌浏览器打开百度。
  - 点击按钮后执行 JS 填写百度搜索框。
  - 示例参考火山“百度填表”逻辑。

- `Cookie设置取出`
  - 内嵌浏览器打开百度。
  - 提供六个 Cookie 操作按钮：
    - `取Cookie_url`
    - `取Cookie_全局`
    - `置Cookie_全局`
    - `取Cookie`
    - `置Cookie`
    - `删除Cookie`

- `同步辅助类`
  - 内嵌浏览器打开百度。
  - 提供四个同步辅助按钮：
    - `同步执行JS`
    - `同步取源码`
    - `同步取文本`
    - `同步创建浏览器`
  - 注意：纯 C++ 中直接复刻火山 `FBroHsBrowserFrame_ExecuteJavaScriptToHasReturn + FBroHsJsCallback` 会触发 Debug CRT 堆断言。当前示例使用原生 CEF 执行 JS，再读取页面标记作为稳定替代方案。

- `服务器`
  - 内嵌浏览器打开 `http://coolaf.com/tool/chattest`。
  - 点击 `创建服务器` 后调用 `FBroHsServer_CreateServer("127.0.0.1", 8888, 100, ...)`。
  - 示例实现了 HTTP 请求响应、WebSocket 连接接受和消息回显。

- `服务器Web`
  - 内嵌浏览器加载本地桌面 Web 页面。
  - 当前页面内置 `Connect` / `Send Test Message` 按钮，可直接测试 WebSocket 回显。
  - 已验证稳定的实现方式是：
    - `OnWebSocketMessage` 回调中只缓存消息
    - 通过主线程消息异步调用 `FBroHsServer_SendWebSocketMessage`
  - 这可以避免在 WebSocket 消息回调上下文中同步回发导致的堆损坏问题。

- `创建URL请求`
  - 内嵌浏览器打开百度。
  - 提供两个 URL 请求按钮：
    - `创建全局URL请求`
    - `创建框架URL请求`
  - 请求地址参考火山示例：`https://api.fbrowser.site:8443/`。
  - 当前稳定实现使用原生 CEF `CefURLRequest::Create`：
    - 全局请求使用空 `RequestContext`
    - 框架请求使用当前浏览器的 `RequestContext`
  - 注意：纯 C++ 直接使用 `FBroHsURLRequest_Create + 手写 FBroHsURLRequestClient` 或 `frame->CreateURLRequest` 会触发 Debug CRT 堆断言。
  - 示例会累加 `OnDownloadData` 数据，并在请求完成后输出完整响应体。

- `JS交互`
  - 使用 `FBroHsGetDataURI("text/html", html)` 创建内嵌 HTML 页面。
  - 页面模拟火山示例中的 `cefQuery` / `cefQuerytest` 两个 JS 交互入口。
  - 当前稳定实现使用 `OnConsoleMessage + ExecuteJavaScript` 完成 JS 与 C++ 双向通信。
  - 注意：没有直接使用 `FBroHsQueryFunctions + 手写 FBroHsQueryHandler`，避免纯 C++ callback 生命周期和火山运行时不一致导致的堆断言风险。

- `拦截获取简单示例`
  - 内嵌浏览器打开百度。
  - 自动监听页面加载过程中的所有资源响应，并把响应体保存到 exe 旁边的 `CapturedResources` 目录。
  - 文件后缀会根据 DevTools 返回的 `mimeType` 和 URL 后缀自动匹配，例如 `.js`、`.png`、`.css`、`.woff2`、`.svg`、`.json`。
  - 当前稳定实现使用 DevTools Protocol 的 `Network.responseReceived` / `Network.loadingFinished` / `Network.getResponseBody`。
  - 注意：纯 C++ 直接返回 `CefResponseFilter` 或复刻火山 `FBroHsResponseFilter` 会触发 Debug CRT 堆断言，因此本项目不走 FBro ResponseFilter 回调链。

- `篡改资源实例`
  - 内嵌浏览器打开百度。
  - 自动拦截 `https://www.baidu.com/` 首页请求，并把百度首页替换为本地自定义 HTML。
  - 当前稳定实现使用 DevTools Protocol 的 `Fetch.enable` / `Fetch.requestPaused` / `Fetch.fulfillRequest`。
  - 这对应火山“资源篡改实例”中通过 `GetResourceHandler` 返回自定义 HTML 的效果，但避免纯 C++ 手写 FBro ResourceHandler 回调导致的堆分配风险。

## 重要说明：不提供 deps.zip

本仓库不提供 `deps.zip`，也不提交 `third_party/fbro` 依赖目录。

原因是 FBro 官方模块会持续更新，CEF 运行时和 `.lib/.dll` 文件体积很大，也可能随版本变化。为了避免仓库过大和依赖过期，请从 FBro 官方发布包或你本机已安装的火山 FBrowser 模块中获取依赖。

VIP 授权码也不会提交到仓库。需要测试 VIP 功能时，请复制 `.env.example` 为本地 `.env`，再填写自己的授权码。

## 依赖目录结构

请在仓库根目录创建：

```text
third_party/
  fbro/
    FBrowserCEF3lib/
    lib64/
    CEFLib64/
```

目录来源通常对应火山模块安装包：

```text
FBrowser/src/env/FBrowserCEF3lib  -> third_party/fbro/FBrowserCEF3lib
FBrowser/src/env/lib64            -> third_party/fbro/lib64
FBrowser/src/CEFLib64             -> third_party/fbro/CEFLib64
```

## 构建

需要：

- Windows
- Visual Studio 2022，MSVC x64
- CMake 3.20+
- FBro / FBrowser CEF 64 位依赖

命令：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
```

生成的解决方案：

```text
build/FBroCppSuite.sln
```

## 运行

构建后每个项目会自动把 `third_party/fbro/CEFLib64` 复制到对应 exe 输出目录。

示例输出：

```text
build/NativeFBroDemo/Debug/NativeFBroDemo.exe
build/BaiduFormFill/Debug/BaiduFormFill.exe
build/Cookie设置取出/Debug/CookieSettingsDemo.exe
build/同步辅助类/Debug/SyncHelperDemo.exe
build/服务器/Debug/ServerDemo.exe
build/服务器Web/Debug/ServerWebDemo.exe
build/创建URL请求/Debug/URLRequestDemo.exe
build/JS交互/Debug/JSInteractionDemo.exe
build/拦截获取简单示例/Debug/ResourceInterceptDemo.exe
build/篡改资源实例/Debug/ResourceTamperDemo.exe
```

## 编码约定

为了避免 MSVC 当前代码页导致中文乱码，C++ 源码中的界面中文建议使用 Unicode 转义，例如：

```cpp
L"\u767e\u5ea6\u586b\u8868"
```

嵌入 HTML 页面时，中文内容也可以使用 HTML 实体，例如：

```html
JS&#20132;&#20114;&#27979;&#35797;
```

## 许可证

本仓库仅包含示例源码和构建配置。FBro / FBrowser、CEF 以及相关运行时文件的授权和分发规则请遵循其官方许可。
