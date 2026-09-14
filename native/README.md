# MotionWallpaper 原生工程

本目录包含 C++/WinRT + WinUI 3 设置应用、原生策略 Agent、Media Foundation Renderer、公共基础设施和原生测试。

```text
<程序目录>\
├─ MotionWallpaper.exe
├─ motionwallpaper-agent.exe
├─ motionwallpaper-renderer.exe
├─ Wallpapers\Groups\...\        # 默认位置，可从应用迁移到外置目录
└─ Config\
   ├─ settings.json
   └─ runtime.json
```

`MotionWallpaper.exe` 是唯一面向用户的可执行文件。Agent 和 Renderer 是内部辅助程序：设置窗口关闭后 Agent 继续驻留；需要播放视频、静态图片或保留冻结画面时 Renderer 才运行。

现有分组、设置和导入媒体会在原位置继续使用。媒体库迁移到程序目录外后，卸载器不会删除该外置目录。

## 构建要求

- Visual Studio 2026（18.x）或对应 Build Tools
- 使用 C++ 的桌面开发
- Windows 应用开发 / WinUI 工具
- MSVC v145 x64/x86 生成工具与 Redistributable 文件
- Windows 10/11 SDK 10.0.19041 或更高版本

执行 `scripts\build-native.ps1`。NuGet 只恢复项目所需的稳定依赖：

- Microsoft.WindowsAppSDK.Foundation 1.8.260803002
- Microsoft.WindowsAppSDK.InteractiveExperiences 1.8.260708001
- Microsoft.WindowsAppSDK.WinUI 1.8.260803003
- Microsoft.Windows.CppWinRT 3.0.260715.1
- Microsoft.Windows.SDK.BuildTools 10.0.28000.2526

可执行文件会发布到现有 `build` 目录，并保留该目录中的默认 `Wallpapers` 与 `Config` 数据。发布前脚本会运行 `MotionWallpaper.Tests.exe`。

项目已经移除 Windows App SDK 2.x Engineering Preview 引用。为控制自包含负载大小，没有引入未使用的 AI、ML、Widgets 和 DWrite 组件。分发二进制负载前请阅读 `../THIRD_PARTY_NOTICES.md` 和 `../docs/RELEASING.md`。

`scripts\build-native.ps1` 通过 `vswhere` 查找 Visual Studio 18 工具链，恢复依赖、构建全部原生项目，并将自包含 WinUI 运行时与当前 v145 x64 Visual C++ Runtime 发布到可执行文件旁。分发目标电脑不需要全局安装 VC++ Redistributable。

脚本固定使用单个、不复用的 MSBuild 节点（`/m:1 /nr:false`）。Visual Studio 18.9 的解决方案级 NuGet Restore 在较高并行度下可能递归扩张工作节点并无诊断失败；这里接受较慢的完整构建，以换取开发机和 CI 上可重复、不会遗留节点的发布过程。

传入 `-SkipPublish` 可以只恢复、构建和运行测试，不生成应用负载或下载 FFmpeg；CI 使用该模式。
