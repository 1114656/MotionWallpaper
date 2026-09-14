# 发布清单

MotionWallpaper 支持 `x.y.z-alpha.n` 预发布版本和 `x.y.z` 正式版本。WinUI 依赖固定在稳定的 Windows App SDK 1.8 维护版本。安装器和便携包必须经过 CI、内容、安装/卸载和校验和验证；未完成代码签名时，无论版本通道如何都必须明确提示 Windows SmartScreen 风险。

当前版本从仓库根目录的 `VERSION` 读取。单 EXE 安装器由 `scripts\build-installer.ps1` 生成，便携包由 `scripts\package-release.ps1` 生成；版本标签推送成功后，`Release` 工作流会重新构建、测试、打包，并根据 VERSION 自动创建预发布版或正式版。Alpha 版本的 Windows 文件版本第四段使用 `0` 至 `65534`，稳定版本保留使用 `65535`，确保同一 `x.y.z` 下正式版的数字版本高于全部 Alpha 版本。

## 源码发布

1. 确认 `git status --short` 干净。
2. 执行 `scripts\build-native.ps1 -SkipPublish`，要求零错误且全部原生测试通过。
3. 检查暂存差异中是否包含设置、日志、媒体、绝对本机路径、凭据、生成文件或二进制文件。
4. 确认包含 `LICENSE`、`SECURITY.md`、`CONTRIBUTING.md` 和 `THIRD_PARTY_NOTICES.md`。
5. 在 GitHub 仓库设置中启用私密漏洞报告。
6. 推送提交并等待 Native CI 通过。
7. 创建与 `VERSION` 完全一致的标签，例如 `v0.1.0-alpha.5` 或 `v0.1.0`；已发布标签不得移动或覆盖。

## 公开分发物

1. 完整执行 `scripts\build-native.ps1` 发布流程。
2. 执行 `scripts\package-release.ps1` 和 `scripts\build-installer.ps1 -SkipBuild`，要求两种分发物验证通过。
3. 确认两种负载只包含简体中文和英文资源，且不包含 `Wallpapers`、`Config`、日志、调试符号和本机测试证据；同时必须包含 `msvcp140.dll`、`msvcp140_atomic_wait.dll`、`vcruntime140.dll` 和 `vcruntime140_1.dll`。
4. 确认主程序、Agent、Renderer 和安装器的 `FileVersion` 为 VERSION 对应的四段数字版本，`ProductVersion` 保留完整版本文本。
5. 在 Windows 10 2004（19041）或更高版本、未全局安装 VC++ Redistributable 的干净 x64 环境中启动安装版和便携版，验证 App-local 运行库生效。
6. 静默安装到临时自定义目录，验证程序、设置、日志与默认壁纸数据集中位于 `App`，随后测试旧数据迁移、升级和卸载。
7. 确认安装阶段先把旧 LocalAppData 复制到非权威 staging，再以路径、类型、64 位大小和 SHA-256 双向校验，完全一致后才原子发布；旧源始终作为回滚副本保留。复制中断或暂时无法完成时必须启用 `legacy-data-fallback.mode` 并继续使用可验证的旧数据根；junction/reparse point、两个非一致数据根等歧义必须写入 `legacy-data-conflict.mode`，App/Agent 均不得自动选择、遍历或写入任一数据根，也不得自动启动半迁移安装。卸载会删除默认 `App\Wallpapers`、设置、日志和开机启动注册表值，但不递归删除任何安装目录外路径。还应把旧 LocalAppData 路径重新用作外置媒体库后再卸载，确认其中数据完整保留。
8. 上传安装 EXE、便携 ZIP 及匹配的 `.sha256` 文件；仅 Alpha 版本标记为 Pre-release，稳定版本创建普通 GitHub Release。
9. 未签名时明确提醒用户注意 SmartScreen；安装版卸载会清除安装目录内的默认媒体库与配置，但保留安装目录外的媒体库，便携版升级时需保留 `Wallpapers` 与 `Config`。

## 正式版外部验证

正式版对外分发前还应完成下列无法由仓库内自动测试完全替代的验证；若某项未完成，发布说明必须明确记录：

1. 确认锁定依赖中没有 Preview 或 Engineering Preview 版本。
2. 重新审计全部 NuGet 依赖和随附工具的许可证。
3. 在干净电脑上验证已构建分发物；构建本身需要 Visual Studio 2026（18.x）、MSVC v145 与对应 Redistributable 文件，目标电脑不应要求这些开发工具或全局 VC++ Runtime。
4. 确认负载包含 `LICENSE.txt`、`THIRD_PARTY_NOTICES.md`，以及 `Tools/ffmpeg` 下的 FFmpeg 声明、LGPL 许可证和 `LICENSE-OpenH264.txt`；构建脚本会校验仓库内 OpenH264 许可证副本的 SHA-256。
5. 固定 BtbN 构建启用了 OpenH264。正式分发前由发布者确认适用地区的 H.264 专利许可要求；Cisco 对其官方预编译二进制的专利许可不应被推定为覆盖 BtbN 自建二进制。
6. 测试安装、升级、卸载、媒体保留、多显示器变化、睡眠、锁定/解锁、关闭显示器恢复和 Explorer 重启。
7. 扫描最终压缩包并公开 SHA-256 校验值。
8. 完成代码签名，并验证 Windows SmartScreen 与杀毒软件误报情况。
