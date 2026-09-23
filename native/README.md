# MotionWallpaper 原生工程

本目录包含 C++/WinRT + WinUI 3 设置应用、原生策略 Agent、Media Foundation Renderer、公共基础设施和原生测试。

发布最低系统基线为 Windows 10 22H2（build 19045）或 Windows 11，需可用的 Media Foundation 和当前用户可写的数据目录。Windows N 须先安装系统可选功能 Media Feature Pack 并重启；最低版本声明不代表已完成全部系统版本的实机验证。

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
- Windows 10/11 SDK（运行时最低版本统一为 Windows 10 22H2 / 10.0.19045；当前构建依赖由工程锁定）

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

传入 `-SkipPublish` 可以只恢复、构建和运行测试，不发布应用负载。该模式仍调用 `prepare-ffmpeg.ps1`，把固定版本的 FFmpeg / ffprobe 和依赖放到测试可执行文件旁的 `Tools\ffmpeg`，供真实媒体集成检查使用；缓存缺失时会下载，CI 同样需要准备网络或有效缓存。

## 视频管线与回归

源探测由随包 ffprobe 完成，封面由有界 FFmpeg 子进程生成；输入不再以 Windows Media Foundation 能否解析源格式作为唯一入口条件。每个转码候选先编码约 2 秒片段，单次预检编码限制为 45 秒，并检查输出规格及 Media Foundation 首帧。首帧探针运行在独立子进程中，10 秒后可终止；完整输出仍需再次验收，再原子发布。

v6 性能副本统一输出 H.264、8-bit 4:2:0、BT.709 SDR、有限范围；PQ/HLG 源经过线性化、色调映射与色域转换，普通 SDR 高位深素材降到 8-bit。最终档位规则为：平衡匹配显示器物理分辨率，帧率目标为 `min(源帧率, 显示器 Hz + 30)`；节能最高 1080p，帧率目标为 `min(源帧率, 显示器 Hz)`，不固定为 60 FPS。低规格输入不放大，低帧率输入保留有理数帧率而不补帧。

“原画直出”直接播放源文件，失败后展示详情及生成平衡/节能副本的操作；用户点击后才创建转换请求并应用所选副本，不静默生成兼容副本代替原画。内部 CPU 兼容预算仍为最高 1080p60 或更低，不能用于静默改写平衡/节能的目标；所选目标超出当前编码或播放能力时必须报告失败。详细规则见 [性能副本设计](../docs/WALLPAPER_PERFORMANCE_AND_VARIANTS.md)。

构建完成后，从仓库根目录运行真实短片段回归：

```powershell
.\scripts\test-video-pipeline.ps1
# 使用指定的测试负载时：
.\scripts\test-video-pipeline.ps1 -TestExecutable 'D:\build\MotionWallpaper.Tests.exe' -Ffmpeg 'D:\build\Tools\ffmpeg\ffmpeg.exe'
```

脚本生成两秒 10-bit SDR、PQ、HLG v210 MOV，调用真实转码器，验收 H.264/BT.709 标签、尺寸、帧率、时长、源文件哈希及完整解码；每次进程调用默认有 120 秒超时。输出目录保留样本、日志及 `summary.json`。平均亮度检查只用于排除近乎全黑/全白，不属于视觉色彩认证，也不能证明某厂商硬编已通过。

显式软件解码不向 Media Engine 提供 DXGI manager；CPU 输出经 WIC BGRA 上传到物理 GPU 呈现，物理设备不可用时才回退 WARP。该路径的缩放和视频处理仍可能在 CPU 执行。同一显卡上的相同媒体同步播放时共享一次解码，各屏通过独立的非阻塞交换链按各自刷新节奏呈现。

当前支持策略与需要补齐的实机矩阵见 [视频运行环境](../docs/VIDEO_ENVIRONMENT.md)。跨进程断点续编码和完整 GPU 缩放路线尚未实现，不应据此声明已完成这些优化。
