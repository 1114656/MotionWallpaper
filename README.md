# MotionWallpaper

轻量、原生的 Windows 动态壁纸应用，支持多显示器、动态屏保和性能副本。

MotionWallpaper 关注三件事：保留导入素材的源画质、让桌面与屏保之间平滑切换，以及尽量降低常驻内存和后台功耗。

> **项目状态：** 当前源码版本为 `v0.1.0` 正式版，使用 Windows App SDK 1.8 维护版本。安装器尚未代码签名，Windows SmartScreen 可能显示警告，详见 [发布清单](docs/RELEASING.md)。

## 主要功能

- 在默认媒体库或用户迁移后的外置媒体库中管理视频和静态图片，并支持自定义分组、排序、重命名和移动。
- 使用视频首帧作为封面；HEVC/Main10 素材可通过 FFmpeg 回退生成封面。
- 支持原画优先、自动平衡和低功耗三种性能档位。
- 自动平衡与低功耗副本在内容相同时共享物理存储，避免重复占用磁盘空间。
- 允许仅删除源视频，同时保留封面、名称、元数据和已有性能副本。
- 使用 Media Foundation、D3D11 和 DirectComposition 在桌面图标后方呈现画面。
- 多显示器可共用壁纸或独立选择；同一显卡上的相同视频只解码一次。
- 暂停、循环、桌面切换和屏保切换期间保留最后一帧，避免闪出系统壁纸。
- 支持全屏窗口自动暂停、闲置动态屏保、锁屏停止和解锁恢复。
- 支持系统托盘、开机启动、随机轮换和按时间间隔切换。
- 导入、设置、元数据和运行状态使用可恢复、原子化的写入流程。
- 删除媒体和分组时使用 Windows 回收站，避免不可恢复的误删。

## 系统要求

- Windows 10 2004（内部版本 19041）或更高版本
- x64 处理器
- 支持 Media Foundation 的显卡驱动

软件解码模式使用 WARP；高分辨率、高帧率素材建议使用硬件解码。安装器与便携包均自带 Windows App SDK 和 x64 Visual C++ Runtime，无需管理员权限或预先安装全局 VC++ 运行库。

## 构建与运行

需要安装 Visual Studio 2026（18.x）或对应 Build Tools，并包含：

- 使用 C++ 的桌面开发
- Windows 应用开发 / WinUI 工具
- MSVC v145 x64/x86 生成工具与 Redistributable 文件
- Windows 10/11 SDK 10.0.19041 或更高版本

在 PowerShell 中执行：

```powershell
.\scripts\build-native.ps1
.\build\MotionWallpaper.exe
```

只构建并运行测试、不生成可运行负载时执行：

```powershell
.\scripts\build-native.ps1 -SkipPublish
```

`MotionWallpaper.exe` 是唯一需要手动启动的程序。它会自动启动常驻策略 Agent；小写命名的 Agent 和 Renderer 可执行文件都是内部组件。

## 安装器与便携包

推荐普通用户下载单 EXE 安装器。安装向导支持简体中文和英文，可选择安装位置，并可选创建桌面快捷方式：

```powershell
.\scripts\build-installer.ps1
```

安装器把运行组件、默认媒体库、配置和日志集中放在所选目录的 `App` 文件夹内。升级会保留并迁移已有数据；卸载会删除程序、配置和仍在 `App\Wallpapers` 中的默认媒体库。用户主动迁移到安装目录以外的媒体库属于外置用户数据，卸载器不会越界删除。

需要免安装使用时，可生成不包含用户壁纸与配置的正式版便携包：

```powershell
.\scripts\package-release.ps1
```

两个脚本都会验证负载，并在 `artifacts` 目录生成中文命名的正式版分发文件和对应的 SHA-256 校验文件。当前分发物尚未代码签名，发布时必须保留 SmartScreen 提示。

## 数据目录

安装版和便携版默认采用单目录数据模式：媒体库存放在 `MotionWallpaper.exe` 同目录的 `Wallpapers`（即安装目录的 `App\Wallpapers`），设置、日志和运行状态存放在 `Config`。侧栏的“移动位置”可以把媒体库复制、校验并切换到其他空文件夹；设置与日志仍留在安装目录。安装版升级时会将旧的 `%LOCALAPPDATA%\MotionWallpaper` 数据复制并校验到程序目录，同时保留旧目录作为回滚副本。卸载安装版只清理默认媒体库与应用配置，不删除当前设置所指向的外置媒体库或旧数据回滚副本；确认新版数据完整后，可由用户自行删除这些外置数据。

便携 ZIP 带有 `portable.mode` 标记；安装器也保留该标记，使两种分发方式使用一致的数据目录。未迁移媒体库时，直接删除便携版目录即可完整移除程序和数据；外置媒体库同样需要由用户单独处理。

删除源视频后，媒体条目不会消失：壁纸列表继续显示封面和名称，自动平衡或低功耗副本仍可播放；原画模式和重新生成副本会变为不可用。

## 项目结构

```text
native/
├─ MotionWallpaper.App       WinUI 3 设置与媒体库界面
├─ MotionWallpaper.Agent     常驻策略与状态协调
├─ MotionWallpaper.Renderer  Media Foundation / D3D11 渲染
├─ MotionWallpaper.Common    公共模型、路径、配置与 Win32 封装
├─ MotionWallpaper.Core      媒体操作、缩略图与转码公共逻辑
└─ MotionWallpaper.Tests     原生自动化测试
```

架构细节见 [架构说明](docs/ARCHITECTURE.md)，性能策略见 [优化路线](OPTIMIZATION.md) 和 [性能副本设计](docs/WALLPAPER_PERFORMANCE_AND_VARIANTS.md)。

## 当前限制

- Windows 安全锁屏界面不能承载自定义 Renderer；`Win+L` 时会停止播放，解锁后重新恢复。
- 当前交换链为 8 位 BGRA，暂不声明原生 10 位/HDR 呈现支持。导入的源文件始终逐字节保留，任何副本都不会改写它；内部 `cpu-smooth` 仅为软件播放的流畅度与兼容性生成 8-bit SDR H.264 副本，可将普通 SDR 10-bit 素材转换为该格式，但不会为 HDR、BT.2020 或带 HDR 色彩标记的高位深素材生成该副本。
- 安装器和便携包尚未进行代码签名，Windows SmartScreen 可能显示警告。
- Media Engine 不提供最终解码器变换名称，因此暂时无法展示具体厂商和型号级解码遥测。

## 参与贡献与安全

提交修改前请阅读 [贡献指南](CONTRIBUTING.md)。安全漏洞请按照 [安全策略](SECURITY.md) 私下报告，不要创建公开 Issue。

## 许可证

MotionWallpaper 自有源代码采用 [MIT License](LICENSE)。第三方组件适用各自的许可证，详见 [第三方软件声明](THIRD_PARTY_NOTICES.md)。
