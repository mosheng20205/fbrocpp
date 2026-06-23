# FBro VIP 原生 C++ 测试结论

## 最新结论

原生 C++ 可以成功调用 FBro VIP 授权接口。

之前失败的主要现象是 `FBroHsOnlineLicenseControl_SetKey(...)` 长时间不返回，随后 `GetError()` 显示 `Couldn't resolve host name`。再次测试后，授权接口已经成功返回，说明之前的问题更像是网络、防火墙、DNS 或授权服务器访问异常，而不是原生 C++ 调用方式本身不可行。

## 最新成功测试结果

本次测试从 `.env` 读取 VIP 授权码，并调用：

```cpp
FBroHsOnlineLicenseControl_SetKey(...)
```

测试日志显示：

```text
VIP license key loaded from .env, calling FBroHsOnlineLicenseControl_SetKey synchronously.
VIP license key loaded from .env, length=64, SetKeyResult=1
LicenseReadyBeforeInit=1
FBrowserVIP.dll availability test started.
FBroBrowser_IsLicenceKey=0
MachineCode=259F8A82-02AF2EA1-E28DD51F-16C3486E-3B5F2C15-A04CF640-06D5C210
LicenseStartDate=2023-11-06 10:32:33
LicenseEndDate=2297-08-20 10:32:33
LicenseFunction=FingerPrint,wss,高级功能,内核开关
LicenseVersion=易语言,火山,CSharp
LicenseType=永久版
LicenseTargetPlatform=x86,x64
FBroHsBrowser_GetVIPControl IsNULL=0
```

其中 `FBroHsBrowser_GetVIPControl IsNULL=0` 表示 VIP 指纹控制对象已经可用。

## 已验证的 VIP 信息接口

原生 C++ 已经成功调用或验证以下接口：

```cpp
FBroHsOnlineLicenseControl_SetKey(...)
FBroHsOnlineLicenseControl_GetShowLicenseStartDate()
FBroHsOnlineLicenseControl_GetShowLicenseEndDate()
FBroHsOnlineLicenseControl_GetShowLicenseFunction()
FBroHsOnlineLicenseControl_GetShowLicenseDevTool()
FBroHsOnlineLicenseControl_GetShowLicenseType()
FBroHsOnlineLicenseControl_GetShowLicenseSysVersion()
FBroHsOnlineLicenseControl_GetError()
FBroHsBrowser_GetVIPControl(...)
```

## 当前原生 C++ 对齐措施

为了让原生 C++ 更接近火山生成程序的运行环境，工程中保留了以下初始化行为：

```cpp
setlocale(LC_CTYPE, "");
SetCurrentDirectoryW(exe_dir);
CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
WSAStartup(MAKEWORD(2, 2), ...);
FBroSetV8DefaultsHeapSize(4, 2048);
```

授权调用当前放在 `FBroHsInitPro(...)` 之前，这一点与火山生成代码的调用顺序更接近。

## 与火山生成 C++ 的关系

火山生成代码中，`VIP注册_置授权码` 最终也是调用：

```cpp
FBroHsOnlineLicenseControl_SetKey(...)
```

火山程序还会先执行运行时初始化，例如：

```cpp
g_objVolApp.init(...)
gCallStartupMethod()
```

原生 C++ 工程没有完整链接火山运行时，但本次测试证明，在当前依赖和初始化条件下，原生 C++ 仍然可以成功完成 VIP 授权。

## 对之前失败现象的修正

之前曾得到以下失败日志：

```text
FBroHsOnlineLicenseControl_SetKey did not return within 30 seconds.
LicenseError=Couldn't resolve host name
FBroHsBrowser_GetVIPControl IsNULL=1
```

现在需要修正该判断：

```text
这不是原生 C++ 必然无法授权。
更可能是当时网络、防火墙、DNS 或授权服务器访问异常。
网络恢复后，原生 C++ 已成功取得 VIP 授权信息。
```

## 注意事项

1. VIP 授权码应保存在 `.env` 中，不要提交到 Git 仓库。
2. `.gitignore` 应继续忽略 `.env` 和 `.env.*`。
3. 如果以后再次出现 `Couldn't resolve host name`，优先检查网络、防火墙、DNS、代理和授权服务器连通性。
4. 如果日志中文乱码，只是日志编码/查看方式问题，不代表授权失败。
5. 判断授权是否成功，应重点看：

```text
SetKeyResult=1
LicenseReadyBeforeInit=1
LicenseStartDate 有值
LicenseEndDate 有值
FBroHsBrowser_GetVIPControl IsNULL=0
```

## 最终判断

当前项目已经证明：

```text
FBro 基础 DLL 可以加载；
机器码接口可以调用；
VIP 授权接口可以成功调用；
VIP 授权信息可以读取；
VIP 指纹控制对象可以获取；
原生 C++ 调用 FBro VIP 接口是可行的。
```
