# 1.0.0 构建验证记录

日期：2026-09-23。功能基线为 0.2.1，本次仅调整版本资源和发布文档。

当前状态：1.0.0 本地正式交付物已生成，发布构建、119 项原生回归、打包及隔离安装验证全部通过。

## 交付物

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| MotionWallpaper-正式版-v1.0.0-Windows-x64-安装包.exe | 66269385 | `cfdc63bc842e664f0bcb4ad6f2e37a5c10cc2778a665bb6084bf8a89bd9ac1ec` |
| MotionWallpaper-正式版-v1.0.0-Windows-x64-便携版.zip | 94087467 | `5be4657448a65d26f901c123b78cf35440bcf09efeab5b8331c3247950686dba` |

以上为本地构建产物，均位于仓库 `artifacts`，附独立 `.sha256` 文件；最终文件哈希与校验文件一致。GitHub Release 会在 CI 环境重新构建，公开下载文件的哈希以该 Release 随附的 `.sha256` 为准。

## 检查结果

- `scripts/build-native.ps1`：Release x64 构建通过，无编译警告/错误，119 项原生测试通过。
- 主程序、Agent、Renderer、安装器产品版本均为 `1.0.0`，Windows 文件版本为 `1.0.0.65535`；安装器数字版本及去除填充空格后的文本版本一致。
- `scripts/package-release.ps1`、`scripts/build-installer.ps1 -SkipBuild`：正式通道命名、内容、版本、依赖、语言资源和许可证检查通过；分发包不含用户壁纸、配置、日志及调试符号。
- `scripts/test-installer.ps1`：隔离目录的媒体组件检查、不可写目录拒绝、改目录升级阻止、同名进程/启动项隔离、中英文资源、App-local VC++ Runtime、版本资源、单目录数据、迁移中断恢复、事务所有权保护、双标记重试、junction 拒绝、外置媒体保留和卸载清理全部通过。
- `git diff --check` 通过。
- 安装包 Authenticode 状态为 `NotSigned`。发布说明保留 SmartScreen 提示和未完成的实机兼容性范围。

本次构建和安装检查使用发布目录及独立临时目录，没有覆盖用户现有安装和媒体库。正式安装的升级方式见 [发布说明](RELEASE_1.0.0.md)。

历史功能测试的具体条件与限制保留在 [0.2.1 验证记录](RELEASE_0.2.1_LOCAL_VALIDATION.md)，不把本次版本号变更视为新的跨硬件认证。
