# FBroC++ 示例工程

中文 | [English](README.en.md)

这是一个使用 C++ 调用 FBro / FBrowser CEF 模块的 Visual Studio / CMake 示例集合。项目来自火山 FBrowser 示例的 C++ 化整理，目标是把常用功能拆成独立、可运行、可学习的小项目。

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
```

## 编码约定

为了避免 MSVC 当前代码页导致中文乱码，C++ 源码中的界面中文建议使用 Unicode 转义，例如：

```cpp
L"\u767e\u5ea6\u586b\u8868"
```

## 许可证

本仓库仅包含示例源码和构建配置。FBro / FBrowser、CEF 以及相关运行时文件的授权和分发规则请遵循其官方许可。
