# 参与 MotionWallpaper 开发

感谢你帮助改进 MotionWallpaper。项目仍在快速迭代，包含可复现步骤、硬件信息和显示器信息的问题报告尤其有价值。

## 提交 Issue 前

- 搜索是否已有相同问题。
- 使用最新的 `main` 分支提交复现。
- 不要上传私人视频、用户设置、日志或包含个人信息的截图。
- 安全漏洞请按照 `SECURITY.md` 私下报告，不要创建公开 Issue。

一份有效的问题报告应包含：

- Windows 版本与内部版本号；
- CPU、GPU 型号和驱动版本；
- 显示器数量、分辨率、刷新率、DPI 缩放和主显示器；
- 使用的解码模式与壁纸性能档位；
- 准确的播放状态和复现步骤；
- 使用其他可合法分享的样例视频时是否仍会出现。

## 开发环境

安装 Visual Studio 2026（18.x）或对应 Build Tools，并包含：

- 使用 C++ 的桌面开发；
- Windows 应用开发 / WinUI 工具；
- MSVC v145 x64/x86 生成工具与 Redistributable 文件；
- Windows 10/11 SDK 10.0.19041 或更高版本。

构建并运行所有原生测试：

```powershell
.\scripts\build-native.ps1
```

只验证构建和测试，不生成可运行负载或下载可选 FFmpeg 后端：

```powershell
.\scripts\build-native.ps1 -SkipPublish
```

## Pull Request 要求

- 每个 Pull Request 只处理一个明确的行为或重构目标。
- 状态机、配置、媒体库、Renderer 协议或并发行为发生变化时，增加或更新自动化测试。
- 除非用户明确要求生成性能副本，否则不得修改导入源文件的画质。
- 策略决策应集中在状态归约器中，避免在 UI、Agent 和 Renderer 中重复实现。
- 路径、JSON、句柄、命令协议和媒体操作优先复用 Common/Core 公共实现。
- 不要提交生成文件、NuGet 包、二进制文件、构建输出、用户设置、日志、缩略图或壁纸媒体。
- 构建完成后确认 `git status --short` 干净。

提交贡献即表示你同意按照 `LICENSE` 中的 MIT License 授权该贡献。
