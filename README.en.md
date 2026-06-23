# FBroC++ Samples

[中文](README.md) | English

This repository contains C++ examples for using the FBro / FBrowser CEF module with Visual Studio and CMake. The samples are organized from Volcano FBrowser demos into small native C++ projects.

## Screenshots

### Browser Creation

![Browser Creation](image/CreateBrowser.png)

### Baidu Form Fill

![Baidu Form Fill](image/FromFill.png)

### Cookie Settings Demo

![Cookie Settings Demo](image/Cookie.png)

## Projects

- `NativeFBroDemo`
  - Demonstrates two browser creation modes:
    - Embedded FBro browser bound to a component `HWND`
    - Native CEF/Chrome popup with `SetAsPopup(nullptr, ...)`
  - Includes basic VIP / VIPControl test calls.

- `BaiduFormFill`
  - Opens Baidu in an embedded browser.
  - Fills the Baidu search box via JavaScript when clicking the button.

- `Cookie设置取出`
  - Opens Baidu in an embedded browser.
  - Provides six Cookie operation buttons:
    - `取Cookie_url`
    - `取Cookie_全局`
    - `置Cookie_全局`
    - `取Cookie`
    - `置Cookie`
    - `删除Cookie`

## Important: deps.zip Is Not Included

This repository does not include `deps.zip` or the `third_party/fbro` dependency directory.

FBro is updated by its official maintainers, and the CEF runtime plus `.lib/.dll` files are large and version-sensitive. To keep the repository lightweight and avoid stale dependencies, please obtain the dependencies from the official FBro package or from your local Volcano FBrowser module installation.

VIP license keys are not committed either. To test VIP features, copy `.env.example` to a local `.env` file and put your own key there.

## Dependency Layout

Create this directory layout at the repository root:

```text
third_party/
  fbro/
    FBrowserCEF3lib/
    lib64/
    CEFLib64/
```

Typical mapping from the Volcano module package:

```text
FBrowser/src/env/FBrowserCEF3lib  -> third_party/fbro/FBrowserCEF3lib
FBrowser/src/env/lib64            -> third_party/fbro/lib64
FBrowser/src/CEFLib64             -> third_party/fbro/CEFLib64
```

## Build

Requirements:

- Windows
- Visual Studio 2022 with MSVC x64
- CMake 3.20+
- 64-bit FBro / FBrowser CEF dependencies

Commands:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
```

Generated solution:

```text
build/FBroCppSuite.sln
```

## Run

After build, each project copies `third_party/fbro/CEFLib64` into its executable output directory.

Example outputs:

```text
build/NativeFBroDemo/Debug/NativeFBroDemo.exe
build/BaiduFormFill/Debug/BaiduFormFill.exe
build/Cookie设置取出/Debug/CookieSettingsDemo.exe
```

## Encoding Rule

To avoid Chinese text corruption under the current MSVC code page, prefer Unicode escapes in C++ UI strings:

```cpp
L"\u767e\u5ea6\u586b\u8868"
```

## License

This repository only contains sample source code and build configuration. Follow the official license terms for FBro / FBrowser, CEF, and related runtime files.
