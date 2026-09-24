# MotionWallpaper

轻量、原生的 Windows 动态壁纸应用，支持多显示器、动态屏保和存储与节能优化。

MotionWallpaper 优先保证稳定播放与日常省电。导入的源文件保留不变，日常使用可生成规格受限的 SDR 性能副本；“原画直出”仍直接使用源文件。

> **项目状态：** 当前版本为 **v1.0.0 正式版**，以已验证的 0.2.1 功能为基线，使用 Windows App SDK 1.8 维护版本。系统缺少视频解码器时，可回退到程序随附的 HEVC/H.264 解码器，无需安装全局解码器。功能、升级方法与已知限制见 [1.0.0 发布说明](docs/RELEASE_1.0.0.md)。尚未完成多厂商显卡和干净系统矩阵验证；安装器尚未代码签名，Windows SmartScreen 可能显示警告，详见 [发布清单](docs/RELEASING.md)。

## 主要功能

- 在默认媒体库或用户迁移后的外置媒体库中管理视频和静态图片，并支持自定义分组、排序、重命名和移动。
- 使用随包提供的 ffprobe 检查视频、FFmpeg 生成封面，探测与封面进程均有超时限制。
- 桌面静态预览按需从原视频提取最高 4K 的 sRGB 图片，HDR 转为 SDR，已有壁纸自动适配；列表保留小缩略图，高清图使用独立磁盘缓存。
- 支持原画直出、自动平衡和低功耗三种性能档位；平衡匹配显示器物理分辨率和刷新率，节能最高 1080p、帧率跟随刷新率，两者都不超过源帧率，不放大低规格源或插帧。
- 原画直接播放源文件；失败时保留源文件和失败详情，由用户点击“生成平衡副本”或“生成节能副本”后才转码并应用。
- 新生成的性能副本统一为 8-bit H.264 / BT.709 SDR；PQ、HLG 输入经过实际色调映射，原文件不被改写。
- 自动平衡与低功耗副本在内容相同时共享物理存储，避免重复占用磁盘空间。
- 允许仅删除源视频，同时保留封面、名称、元数据和已有优化副本。
- 使用 Media Foundation、D3D11 和 DirectComposition 在桌面图标后方呈现画面；系统缺少解码器时可回退到随包 HEVC/H.264 解码器，内置路径输出 sRGB SDR。
- 多显示器可共用壁纸或独立选择；同一显卡上的相同视频同步播放时共享一次解码，各屏通过独立呈现队列跟随各自刷新节奏。
- 每块屏幕独立显示正在应用、已应用、已暂停、正在优化、降级播放或失败，并可重试或重启渲染。
- 暂停、循环、桌面切换和屏保切换期间保留最后一帧，避免闪出系统壁纸。
- 支持全屏窗口自动暂停、闲置动态屏保、立即预览、输入即时唤醒、锁屏停止播放和解锁恢复；接通电源时副本生成可跨屏保、锁屏及熄屏继续。
- 支持搜索、收藏、标签、筛选、多选整理、重复项目修复以及媒体库备份和恢复。
- 支持工作、夜间、电池和投屏场景，并可在显示器布局图中直接分配壁纸。
- 系统托盘支持暂停/恢复、下一张、立即屏保和当前状态；同时支持开机启动与随机轮换。
- 导入、设置、元数据和运行状态使用可恢复、原子化的写入流程。
- 删除媒体和分组时使用 Windows 回收站，避免不可恢复的误删。

## 系统要求

- Windows 10 22H2（内部版本 19045）或 Windows 11
- x64 处理器
- 可用的 Windows Media Foundation 播放环境；Windows N 需先通过系统“可选功能”安装 Media Feature Pack 并重启。显卡及驱动能力在运行时探测
- 当前用户可写的安装目录或便携目录，用于配置、日志及默认媒体库

上述 Windows 版本是当前兼容目标，不代表全部版本和显卡组合均已实机认证。程序没有强制某一显卡型号；硬件编码失败时可尝试受限的 OpenH264 软件编码，源视频硬件解码与输出硬件编码分别判断。当前随包 FFmpeg 的 NVIDIA NVENC 路线要求兼容 SDK 13.0 的驱动，Windows 驱动基线为 570 或更高；旧驱动仍可能走 CPU 编码。具体门槛和验证范围见 [视频运行环境](docs/VIDEO_ENVIRONMENT.md)。

系统播放路径的显式软件模式使用 CPU 解码，经 WIC BGRA 上传到优先选择的物理 GPU 呈现；内置解码的 CPU 兼容路径使用 YUV 上传及 GPU 色彩转换。仅在物理设备创建失败后回退 WARP。安装器与便携包均自带 Windows App SDK 和 x64 Visual C++ Runtime，无需预先安装全局 VC++ 运行库。

平衡档在 2K 60 Hz 显示器上以 2K60 为目标，在 1080p 60 Hz 显示器上以 1080p60 为目标；源视频只有 60 FPS 时，两者都只保留 60 FPS。节能档在 144 Hz 显示器上可以生成最高 1080p144，因此不承诺它在高刷新率下始终达到最低功耗。内部 CPU 兼容预算仍是最高 1080p60 或更低，不能把这个预算冒充用户所选档位；环境无法满足目标时应展示失败和详情。

## 构建与运行

需要安装 Visual Studio 2026（18.x）或对应 Build Tools，并包含：

- 使用 C++ 的桌面开发
- Windows 应用开发 / WinUI 工具
- MSVC v145 x64/x86 生成工具与 Redistributable 文件
- Windows 10/11 SDK（运行时最低版本统一为 10.0.19045；当前构建依赖由工程锁定）

在 PowerShell 中执行：

```powershell
.\scripts\build-native.ps1
.\build\MotionWallpaper.exe
```

只构建并运行测试、不生成可运行负载时执行：

```powershell
.\scripts\build-native.ps1 -SkipPublish
```

`-SkipPublish` 仍准备测试所需的 FFmpeg / ffprobe 到测试程序旁的 `Tools\ffmpeg`；缓存不存在时需要下载。构建后可执行 `scripts\test-video-pipeline.ps1`，检查真实 10-bit SDR、PQ、HLG MOV 到 SDR H.264 的短片段流程。

`MotionWallpaper.exe` 是唯一需要手动启动的程序。它会自动启动常驻策略 Agent；小写命名的 Agent 和 Renderer 可执行文件都是内部组件。

日常开发测试无需安装：双击仓库根目录的 `Start-Dev.cmd`，或执行以下命令：

```powershell
.\scripts\start-dev.ps1
# 修改源码后，重新构建、运行测试并启动：
.\scripts\start-dev.ps1 -Rebuild
```

启动脚本会先正常退出其他位置正在运行的 Motion，避免安装版和开发版混用后台服务。开发版固定使用 `build\Config` 和 `build\Wallpapers`，重新构建会保留这两个目录；不要手动删除整个 `build` 目录。已有开发版数据不会从安装版反复覆盖，重复启动会打开同一窗口。首次启动若尚无编译结果，会自动构建。

## 安装器与便携包

推荐普通用户下载单 EXE 安装器。安装向导支持简体中文和英文，可选择安装位置，并可选创建桌面快捷方式：

```powershell
.\scripts\build-installer.ps1
```

安装器把运行组件、默认媒体库、配置和日志集中放在所选目录的 `App` 文件夹内。升级会保留并迁移已有数据；卸载会删除程序、配置和仍在 `App\Wallpapers` 中的默认媒体库。用户主动迁移到安装目录以外的媒体库属于外置用户数据，卸载器不会越界删除。

需要免安装使用时，可生成不包含用户壁纸与配置的当前版本便携包：

```powershell
.\scripts\package-release.ps1
```

两个脚本都会验证负载，并在 `artifacts` 目录生成与 VERSION 通道一致的分发文件和对应的 SHA-256 校验文件。当前分发物尚未代码签名，发布时必须保留 SmartScreen 提示。

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

架构细节见 [架构说明](docs/ARCHITECTURE.md)，性能策略见 [优化路线](OPTIMIZATION.md) 和 [存储与节能优化设计](docs/WALLPAPER_PERFORMANCE_AND_VARIANTS.md)。

## 当前限制

- Windows 安全锁屏界面不能承载自定义 Renderer；`Win+L` 时会停止播放，解锁后重新恢复。
- 当前交换链为 8 位 BGRA，暂不声明原生 10 位/HDR 呈现支持。“原画直出”表示直接读取源文件，不代表已经具备完整 HDR 显示链路；性能副本会把支持的 HDR 输入转换成 SDR。
- 电池供电、用户暂停/取消、进程退出等中断仍会结束当前编码；后续从头生成，不支持跨进程断点续编码。锁屏继续生成不阻止 Windows 自动锁屏、熄屏或睡眠。
- Intel、AMD 与不同代际 NVIDIA 的完整实机兼容矩阵仍待验证；短片段预检和回退不能替代硬件认证。
- 安装器和便携包尚未进行代码签名，Windows SmartScreen 可能显示警告。
- Media Engine 不提供最终解码器变换名称，因此暂时无法展示具体厂商和型号级解码遥测。

## 参与贡献与安全

提交修改前请阅读 [贡献指南](CONTRIBUTING.md)。安全漏洞请按照 [安全策略](SECURITY.md) 私下报告，不要创建公开 Issue。

## 许可证

MotionWallpaper 自有源代码采用 [MIT License](LICENSE)。第三方组件适用各自的许可证，详见 [第三方软件声明](THIRD_PARTY_NOTICES.md)。
