# C++原生调用 FBro

这个目录演示“原生 C++ 直接调用 FBro 底层头文件和 lib”的做法，不依赖火山生成的 `vpkg_*.cpp` 代码。

## 目录内容

- `src/main.cpp`: 最小 C++ 示例，直接调用 `FBroHsInitPro` / `FBroRunMessageLoop` / `FBroShutdown`，并用 CEF 的 `CefBrowserHost::CreateBrowser` 创建浏览器窗口。
- `CMakeLists.txt`: 配置 FBro 头文件、`FBrowserCEF3lib.lib`、`FBrowserVIP.lib`、`libcef.lib`、`libcef_dll_wrapper.lib`。
- `build.ps1`: 一键 CMake x64 Debug 构建脚本。
- [`FBro接口盘点.md`](./FBro接口盘点.md): `FBrowserCEF3lib.dll`、`FBrowserVIP.dll`、C++ 头文件、回调、窗口句柄和消息机制的扫描结论。
- `deps`: FBro/CEF 依赖目录，不进入 Git 仓库。请从 GitHub Releases 下载 `deps.zip` 并解压到项目根目录。

## 获取依赖

克隆仓库后，先从 GitHub Releases 下载 `deps.zip`，解压后目录结构应为：

```text
Native-FBro-C-Starter
├─ CMakeLists.txt
├─ build.ps1
├─ src
└─ deps
   ├─ FBrowserCEF3lib
   ├─ lib64
   └─ CEFLib64
```

如果缺少 `deps`，CMake 会找不到 FBro/CEF 头文件和链接库。

## 构建

在“x64 Native Tools Command Prompt for VS”或已经能调用 MSVC/CMake 的 PowerShell 中执行：

```powershell
Set-Location "<克隆后的仓库目录>"
.\build.ps1
```

输出文件：

```text
build\Debug\NativeFBroDemo.exe
```

## 在 Visual Studio 中调试

推荐使用 Visual Studio 的 CMake 文件夹模式，不需要手动创建 `.sln`：

1. 打开 Visual Studio 2022。
2. 选择“打开本地文件夹”或“文件 -> 打开 -> 文件夹”。
3. 选择克隆后的本目录。
4. 等待 Visual Studio 自动识别 `CMakeLists.txt` 并完成 CMake 配置。
5. 顶部配置选择 `x64-Debug` 或类似的 x64 Debug 配置。
6. 在启动项下拉框中选择 `NativeFBroDemo.exe`。
7. 打开 `src/main.cpp`，在 `wWinMain`、`DemoInitEvent::OnContextInitialized`、`DemoClient::OnLoadEnd` 等位置打断点。
8. 按 `F5` 开始调试。

如果 Visual Studio 没有自动把 `NativeFBroDemo.exe` 设为启动项，可以在菜单中选择“项目 -> 设置启动项”，或者在 CMake 目标列表里右键 `NativeFBroDemo` 后选择“设为启动项”。

调试时要注意工作目录。CEF/FBro 需要在 exe 同目录找到 `locales`、`*.pak`、`*.dat`、`libcef.dll`、`FBroSubprocess.exe` 等运行时文件。本工程的 CMake 会在构建后自动把 `deps\CEFLib64` 复制到输出目录，所以正常情况下不要直接运行中间 obj 目录里的程序，只运行 Visual Studio 生成的目标 exe。

默认输出位置通常是：

```text
build\Debug\NativeFBroDemo.exe
```

如果你在 Visual Studio 中看到 `Unable to find locale data files. Please reinstall.`，先重新生成项目，再检查调试启动的 exe 目录下是否有：

```text
locales\zh-CN.pak
resources.pak
icudtl.dat
libcef.dll
FBroSubprocess.exe
```

如果需要单步进入 FBro/CEF 头文件里的 inline 代码，可以正常进入；但 `FBrowserCEF3lib.dll`、`FBrowserVIP.dll`、`libcef.dll` 本身没有对应源码和 PDB，通常只能调到调用边界。建议把自己的封装逻辑集中写在 `src/main.cpp` 或后续新增的 C++ 类中，断点打在自己的代码和 CEF 回调里。

FBro 依赖应放在本目录的 `deps` 下：

- `deps\FBrowserCEF3lib`: FBro/CEF 头文件。
- `deps\lib64`: x64 链接库。
- `deps\CEFLib64`: x64 运行时文件。

构建后会自动把 `deps\CEFLib64` 下的运行时文件复制到 exe 目录，包括 `FBroSubprocess.exe`、`FBrowserCEF3lib.dll`、`FBrowserVIP.dll`、`libcef.dll`、`locales`、`*.pak`、`*.dat` 等。

如果启动时报 `Unable to find locale data files. Please reinstall.`，优先检查：

- exe 当前目录下是否存在 `locales\zh-CN.pak`。
- `settings.resources_dir_path` 是否指向 exe 目录。
- `settings.locales_dir_path` 是否指向 exe 目录下的 `locales`。
- FBro 当前接口的路径参数是 `char*`，示例里使用系统 ANSI 编码传入，和火山模块生成代码的 `FBroUnit::UnicodeToAnsi` 行为保持一致。

## 关键点

这个示例故意没有使用火山生成目录 `_int` 里的 `vpkg_FBroLib.cpp`、`vpkg_main.cpp`，因为那些文件依赖火山运行时和自动生成的类名。这里保留的是 C++ 可以长期维护的调用边界：

```cpp
FBroInitSettings settings{};
FBroHsInitPro(&settings, init_event, 1024);
CefBrowserHost::CreateBrowser(...);
FBroRunMessageLoop();
FBroShutdown(TRUE);
```

后续要扩展 JS 交互、请求拦截、Cookie、VIP 功能，可以继续包含对应的 `FBro*.h`，或在 `DemoClient` / `DemoInitEvent` 里实现 CEF 回调。

## 推送到 Git 仓库

这个工程可以作为独立仓库推送，但必须注意大文件：

- `deps` 是必需依赖，但不进入 Git 仓库。
- `build` 是本机构建产物，已经在 `.gitignore` 中排除，不需要推送。
- 请把 `deps` 打包为 `deps.zip`，上传到 GitHub Release 附件。

首次建仓并推送前执行：

```powershell
git init
git add .gitattributes .gitignore CMakeLists.txt build.ps1 README.md src
git commit -m "Add native FBro C++ demo"
git remote add origin <你的仓库地址>
git push -u origin main
```

如果默认分支不是 `main`，先执行：

```powershell
git branch -M main
```

别人克隆后下载 Release 附件里的 `deps.zip` 并解压：

```powershell
git clone <你的仓库地址>
Set-Location "<克隆后的仓库目录>"
# 下载 deps.zip 后解压到当前目录，得到 .\deps
.\build.ps1
```

维护者打包依赖时可以执行：

```powershell
Compress-Archive -Path .\deps -DestinationPath .\deps.zip -Force
```

然后在 GitHub 仓库页面创建 Release，把 `deps.zip` 作为附件上传。

克隆后只要机器上有 Visual Studio 2022 C++ 工具链和 CMake，就不需要本机存在火山安装目录或 `T:` 盘。
Set-Location "<克隆后的仓库目录>"
git lfs pull
.\build.ps1
```

克隆后只要机器上有 Visual Studio 2022 C++ 工具链和 CMake，就不需要本机存在火山安装目录或 `T:` 盘。
