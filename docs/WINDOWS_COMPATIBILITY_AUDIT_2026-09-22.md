# Windows 兼容性设计审计

日期：2026-09-22。审查对象是工作区当前代码，包含本日已完成的三档规则、SDR 性能副本与原画恢复改动，不是仅审查远端 HEAD。

审计覆盖 App、Agent、Renderer、视频探测和转码、配置与媒体库、安装升级、依赖打包及 CI 的主要链路。下文保留审计时的发现与位置；其后获准实施的第一批修复及验证结果见 [第一批修复记录](WINDOWS_COMPATIBILITY_FIXES_2026-09-22.md)。未对用户安装执行锁屏、驱动切换或卸载试验。

前一轮的 93 项原生测试和四组视频转换已经通过，证明相应检查在当前机器通过；不能据此声明所有 Windows / GPU 组合兼容。本机读取到 Windows build 26200.9168、DisplayVersion 25H2、x64。已有验证记录中的 GPU 是 GTX 1070、驱动 560.94。其他硬件组合需要独立验收。

## 结论与优先顺序

项目已有值得保留的基础：进程分离、实际首帧确认、随包 VC 运行库和媒体工具、可终止的媒体探测、短片段转码预检、输出验收、原文件保护以及 Explorer / 显示拓扑恢复。

主要缺口在环境准入、驱动故障隔离、多 GPU 候选回退、多屏独立调度，以及安装位置与数据位置的关系。不要用显卡型号或显存大小代替这些能力检查。建议先修 P1 和启动诊断，再修多屏与回退；性能优化放在可靠性之后。

P1 表示应先于扩大兼容性承诺解决的问题；P2 表示特定环境下会影响功能、恢复、性能或可维护性的问题。下述代码事实与尚待实机验证的后果分别说明。

## 1. P1：驱动探测仍在 Agent 策略线程内同步运行

证据：[Agent.cpp:108](../native/MotionWallpaper.Agent/Agent.cpp#L108)、[Agent.cpp:2726](../native/MotionWallpaper.Agent/Agent.cpp#L2726)、[VideoOptimizer.cpp:321](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L321)。

`physical_video_device_available()` 和 `SourceHardwareDecodeAdapter()` 直接调用 D3D11 设备创建与驱动解码能力查询。ffprobe 已有独立超时，但这些驱动调用没有进程隔离或可执行的取消期限。发生驱动挂起时，Agent 可能无法及时处理锁屏、退出和设置变化。这是由调用结构确定的风险，本轮未实机制造驱动挂起。

建议把完整设备探测放进辅助进程，Agent 异步接收结果；超时结束该进程，发布明确原因并使用已知可用路线。以设备、驱动、工具版本和输入类别缓存结果。验收应注入不返回的探测进程，确认 App、退出和电源策略仍能响应。

## 2. P1：固定媒体工具来自短期保留的每日构建

证据：[prepare-ffmpeg.ps1:13](../scripts/prepare-ffmpeg.ps1#L13)、[build-native.ps1:147](../scripts/build-native.ps1#L147)。

脚本固定下载 `autobuild-2026-09-16-19-44`；没有缓存的新机器和 CI 必須下载，测试构建也依赖它。上游说明仅保留最近 14 个每日构建，月末构建另有保留规则。因此这个非月末下载地址不能作为长期可重现的依赖。[构建方保留策略](https://github.com/BtbN/FFmpeg-Builds#release-retention-policy)

建议把当前已验证归档保存到项目控制的长期制品位置，保留 SHA-256、许可证和完整版本清单；不能改成浮动 latest 而失去可重现性。验收从空缓存构建安装包。本轮没有向远端发布任何制品。

## 3. P1：最低 Windows 承诺与随包工具的支持范围不一致

证据：[MotionWallpaper.iss:38](../installer/MotionWallpaper.iss#L38)、[App.vcxproj:19](../native/MotionWallpaper.App/MotionWallpaper.App.vcxproj#L19)。

工程和安装器下限是 Windows 10 build 19041（2004）；当前 FFmpeg 构建方只保证 Windows 10 22H2 及以上。这个差异不证明 19041 必然不能运行，但意味着当前依赖无法支撑项目对旧版本的兼容承诺。[构建方系统要求](https://github.com/BtbN/FFmpeg-Builds)

建议明确二选一：采用并验证支持 19041 的固定工具链，或把发布基线统一到经过验收的较新版本。不要只改一处安装器数字。界面、Agent、Renderer、媒体工具和干净系统安装都要在最低版本跑通。

## 4. P2：Windows N 缺少媒体组件时，缺少启动前诊断

证据：[MotionWallpaper.iss:142](../installer/MotionWallpaper.iss#L142)、[Agent.vcxproj:29](../native/MotionWallpaper.Agent/MotionWallpaper.Agent.vcxproj#L29)、[MainWindow.xaml.cpp:4513](../native/MotionWallpaper.App/MainWindow.xaml.cpp#L4513)。成品依赖检查确认 Agent / Renderer 普通导入表引用 `MFPlat.DLL` 和 `MFReadWrite.dll`。

Windows N 未安装 Media Feature Pack 时，系统媒体组件缺失可使后台进程在进入程序逻辑前就启动失败；当前安装检查和 `StartController()` 没有将这种情况解释给用户。FFmpeg 能转码不能补上 Windows 播放链缺少的系统组件。[微软说明](https://support.microsoft.com/en-us/windows/experience/platform-variants/media-feature-pack-for-windows-n)

建议安装与首次启动检查媒体组件，提示安装官方可选功能；增加 Agent 启动握手、超时和退出码诊断。不要以“视频格式不支持”概括系统组件缺失。

## 5. P2：实际播放失败后没有继续尝试另一张 GPU

证据：[VideoOptimizer.cpp:327](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L327)、[VideoOptimizer.cpp:1007](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L1007)、[Agent.cpp:2728](../native/MotionWallpaper.Agent/Agent.cpp#L2728)。

探测返回并缓存第一张静态能力符合的显卡。该显卡随后发生真实首帧失败时，Agent 清空同一候选并进入 CPU 兼容路线，没有继续选第二张卡。核显或独显驱动报告能力但实际失败的混合显卡机器，会丢失另一张 GPU 本可工作的机会。

建议返回有序候选列表，按 LUID 排除本次已失败设备，逐卡进行有期限的实际播放尝试后再使用 CPU。环境改变后重新评估，避免无限重试同一张卡。

## 6. P2：升级驱动、换显卡或降低目标后，旧失败记录仍阻止生成

证据：[VariantCache.h:716](../native/MotionWallpaper.Common/VariantCache.h#L716)、[VideoOptimizer.cpp:822](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L822)、[VideoOptimizer.cpp:1052](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L1052)。

持久失败标记只写档位名称，`InvalidateChoices()` 只清内存。因此旧驱动导致的 balanced 失败，可以在升级驱动、改变输出目标或重启后继续阻止自动生成，直到用户手动重试。

建议失败记录绑定源指纹、输出规格、GPU / 驱动、媒体工具版本及失败类别。环境改变时只失效不再适用的失败记录，不能顺手清掉用户主动暂停、取消或禁止生成的状态。分别验收确定性不支持与暂时设备忙。

## 7. P2：锁屏与显示电源使用的会话语义不够准确

证据：[Agent.cpp:1441](../native/MotionWallpaper.Agent/Agent.cpp#L1441)、[Agent.cpp:1573](../native/MotionWallpaper.Agent/Agent.cpp#L1573)、[Agent.cpp:1478](../native/MotionWallpaper.Agent/Agent.cpp#L1478)。

锁屏查询只把 `WTSConnectState != WTSActive` 当作锁屏，随后每秒覆盖锁屏事件保存的状态。连接状态与锁定状态不是同一字段，Windows 提供 `WTSInfoEx` 的 `SessionFlags` 表达锁定。在两种状态不同步或会话切换的环境，有误恢复播放或重置锁后熄屏计时的风险；本轮未锁定当前电脑复现。[锁定状态字段](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w)

同时，交互式 Agent 注册的是 `GUID_CONSOLE_DISPLAY_STATE`，微软要求交互会话应用使用 `GUID_SESSION_DISPLAY_STATUS`。远程桌面和用户切换时，这个差异尤其需要处理。[电源通知定义](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids)

建议分离 connected、locked、sessionDisplayOn、remoteSession，未知查询结果保留已知状态。用事件记录回放测试锁定、断开、重连和唤醒，再在实机验证。

## 8. P2：软件解码连带禁用物理 GPU 呈现，CPU 档位又只看线程数

证据：[Renderer.cpp:1229](../native/MotionWallpaper.Renderer/Renderer.cpp#L1229)、[Renderer.cpp:561](../native/MotionWallpaper.Renderer/Renderer.cpp#L561)、[PlaybackCapabilityPolicy.h:60](../native/MotionWallpaper.Agent/PlaybackCapabilityPolicy.h#L60)。

显式 software 模式直接创建 WARP，把输出侧也交给 CPU。视频编码不支持硬解，不代表显卡不能完成缩放和显示。双屏、4K 输出会放大这种成本。与此同时，仅凭逻辑线程数分配 1080p60 / 720p60 等预算，不能代表老 CPU、低功耗 CPU 或混合核心 CPU 的真实吞吐。这是现有设计限制，不是本日新增回归。[WARP 设备说明](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp)

建议把解码设备、编码设备和显示设备分开选择；CPU 解码仍优先物理 GPU 显示，WARP 作最后回退。线程数只用于首次保守估计，后续用实际解码耗时、丢帧和输出规模修正兼容能力，明确展示兼容规格。

## 9. P2：同 GPU 混合刷新率屏幕缺少独立呈现节奏

证据：[SharedRendererPolicy.h:90](../native/MotionWallpaper.Agent/SharedRendererPolicy.h#L90)、[Renderer.cpp:285](../native/MotionWallpaper.Renderer/Renderer.cpp#L285)。

同媒体、同 GPU 的屏幕共享 Renderer，并在一个循环内对每块屏幕串行 `TransferVideoFrame` 和 `Present(1, 0)`。60 Hz 与 144 Hz 组合中，低刷交换链排队满后可能阻塞公共循环，高刷输出无法独立推进。这是根据同步与队列语义得到的高可信推断，不是已测得某个固定 FPS。[微软多交换链说明](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-multiple-swap-chains)

最小改进是将刷新率加入路由分组；长期保留共享解码，但各输出独立调度、独立跳帧。实测至少覆盖 60+144、59.94+119.88、横竖屏和热插拔。

## 10. P2：刷新率精度和交换链延迟配置存在明确缺口

证据：[DisplayTopology.cpp:32](../native/MotionWallpaper.Common/DisplayTopology.cpp#L32)、[Renderer.cpp:470](../native/MotionWallpaper.Renderer/Renderer.cpp#L470)、[Renderer.cpp:483](../native/MotionWallpaper.Renderer/Renderer.cpp#L483)。

显示刷新率来自整数 `dmDisplayFrequency`，无法表示 60000/1001 等时序。建议使用 `QueryDisplayConfig` 的有理刷新率，贯穿目标规格、缓存和呈现；Windows 11 动态刷新率还应区分虚拟与物理时序。[目标时序结构](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_target_info)

交换链没有设置 `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`，却调用交换链的 `SetMaximumFrameLatency(1)` 并忽略返回值。按 API 契约该配置会失败。应正确启用并接入等待对象，或使用适合多交换链的设备级预算，检查返回码。不能在多交换链设备上直接把总预算设为 1。[API 限制](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency)

## 11. P2：换安装目录升级与受保护目录没有完整的数据策略

证据：[MotionWallpaper.iss:35](../installer/MotionWallpaper.iss#L35)、[MotionWallpaper.iss:665](../installer/MotionWallpaper.iss#L665)、[Common.cpp:671](../native/MotionWallpaper.Common/Common.cpp#L671)、[SettingsStore.cpp:44](../native/MotionWallpaper.App/SettingsStore.cpp#L44)。

安装器允许改目录，但迁移只从早期 LocalAppData 数据目录读取，没有搬迁当前旧安装旁的 Config / Wallpapers。换位置升级可能呈现空库，或重新采用旧回滚数据；原数据一般还留在旧目录，不能描述成已被删除。

安装版又强制采用 portable.mode；放入受保护或只读目录时，加载设置后的无条件保存会失败，界面把它归类为读取失败并停用媒体库访问。

建议区分普通安装的每用户数据目录和用户主动选择的便携模式；改变程序安装位置不应改变权威数据。升级读取旧安装位置并校验迁移；便携模式提前检查写权限，给准确错误。

## 12. P2：多份程序共用 IPC，卸载操作没有限定安装位置

证据：[App.xaml.cpp:12](../native/MotionWallpaper.App/App.xaml.cpp#L12)、[Agent.cpp:1894](../native/MotionWallpaper.Agent/Agent.cpp#L1894)、[Common.h:17](../native/MotionWallpaper.Common/Common.h#L17)、[MotionWallpaper.iss:85](../installer/MotionWallpaper.iss#L85)。

所有安装和便携目录共用会话内固定互斥量、通知事件和启动项。旧目录 Agent 驻留而 UI 关闭时，新目录 App 可以启动，但新 Agent 被互斥量拒绝，双方读不同数据目录。卸载器按映像名终止进程，并无条件删除同名 Run 值，会影响其他位置的版本。

建议明确单安装实例交接协议，校验进程路径、版本和数据根；或按实例命名 IPC。卸载只停止本目录的进程，只删除仍指向本目录的启动项。验收安装版与便携版共存、移动目录及卸载其中一份。

## 13. P2：兼容性诊断丢失了最有价值的错误信息

证据：[Renderer.cpp:106](../native/MotionWallpaper.Renderer/Renderer.cpp#L106)、[Agent.cpp:688](../native/MotionWallpaper.Agent/Agent.cpp#L688)、[Agent.cpp:695](../native/MotionWallpaper.Agent/Agent.cpp#L695)。

Renderer 已发送 operation 和 HRESULT，但 Agent 对 error 行只设置布尔失败，不保存原始内容；进程退出也未提取退出码。缺媒体组件、设备被移除、格式不支持和宿主窗口失败，容易最终只剩“渲染进程退出”等泛化原因。

建议保留结构化错误码、阶段、显示器、设备 LUID / 驱动、输入规格、输出目标、实际候选和退出码，提供可预览的诊断导出。不要自动上传用户路径或媒体。界面目前已把硬件状态写为“已请求 DXGI/DXVA 路径”，应保留这种准确措辞；首帧成功不等同于证明厂商硬件解码器已被采用。

## 14. P2：Dolby Vision 需要独立的输入能力边界

证据：[MediaProbe.cpp:299](../native/MotionWallpaper.Common/MediaProbe.cpp#L299)、[VideoTranscodeColorPolicy.h:88](../native/MotionWallpaper.Agent/VideoTranscodeColorPolicy.h#L88)。

当前没有提取 Dolby Vision profile 和兼容层信息；普通 PQ/HLG 色调映射后会去掉 Dolby Vision 侧数据。对没有 HDR10 兼容层的 Profile 5 等输入，存在副本能够解码但颜色错误的风险。此处未用真实 DV 样本复现，不否定已经验证的普通 SDR / PQ / HLG 路线。[Dolby profile 说明](https://professionalsupport.dolby.com/s/article/Dolby-Vision-Encoding-using-Blackmagic-Design-DaVinci-Resolve-Studio-AQs)、[FFmpeg Dolby Vision 处理选项](https://ffmpeg.org/ffmpeg-filters.html#libplacebo)

最小改进是识别并拒绝不支持的 DV profile，显示明确原因；专用转换验收后再增加支持。元数据正确、首帧通过和非全黑检查，均不能代替色彩验收。

## 建议的兼容模型

保留用户已确认的档位：节能最高 1080p、帧率跟随显示器；平衡采用显示器尺寸、刷新率加 30；源规格较低时不放大、不插帧；原画直接播放，失败后由用户选择生成副本。

这些是输出目标。每个目标都应独立检查输入可读、输入可解码、输出可编码、生成结果可播放、显示设备可呈现。超过设备能力时给出可选择的兼容规格和原因，不能把较低规格显示成目标已实现。

| 层次 | 建议承诺方式 |
| --- | --- |
| 基础运行 | 明确最低 Windows build、x64、可写数据目录、媒体组件和安装握手；不以安装成功代替可播放 |
| 普通视频 | 优先已验证的 H.264 8-bit SDR；源 MOV 扩展名不决定解码能力 |
| 高规格视频 | HEVC Main10 / HDR / AV1 按输入 profile、位深、色彩和尺寸分别验证，必要时生成 SDR 副本 |
| 老显卡 | 显示能力、解码能力和编码能力分离；CPU 路线保留 GPU 呈现，给出真实兼容规格 |
| 新显卡 | 驱动版本满足工具 API 只是前提，还需实际目标预检，不能按型号直接判成功 |
| 混合显卡 | 候选逐卡尝试，成功路径绑定 LUID；环境改变后重新检查 |
| ARM64 | 当前只有 x64 成品，安装器 x64compatible 允许模拟运行；应作为独立待验证路线，不能冒充原生 ARM64 认证 |
| RDP / 虚拟显示 | 单独规定暂停或兼容呈现策略，返回本机会话时重新探测；不把重定向适配器当普通本地独显 |

当前 NVENC 工具 API 的驱动门槛和 CPU 回退预算已在 [视频运行环境](../docs/VIDEO_ENVIRONMENT.md) 记录。未满足 2K90 等目标时明确失败，是当前约定；不再把这项约定本身列为缺陷。若要扩大旧驱动覆盖，需要重新选择并验证媒体工具基线。

## 必须补的验收矩阵

[当前 CI](../.github/workflows/native-ci.yml#L18) 只有一个托管 Windows runner。自动化和纯策略断言应保留，但不能替代桌面宿主、真实驱动与视频引擎的组合测试。

| 维度 | 最小覆盖 | 验收重点 |
| --- | --- | --- |
| 系统 | 最终声明的最低版本、Windows 10 22H2、Windows 11 24H2 / 25H2；N 版缺组件及安装组件后 | 从空环境安装、启动、导入、首帧、循环和退出 |
| 显卡 | Intel 核显、AMD、NVIDIA 较老与较新代际、核显+独显 | 区分解码/编码/呈现，记录真正采用的设备和失败原因 |
| 驱动 | 合格版本、低于编码 API 门槛、升级前后、设备失效 | 快速失败、可解释、恢复后不被旧失败缓存锁死 |
| 屏幕 | 单屏、同卡混合刷新率、跨卡、59.94 Hz、4K、不同 DPI、旋转、热插拔 | 高刷屏不被低刷屏阻塞，原生物理像素与有理时序准确 |
| 会话 | 锁定/解锁、睡眠/唤醒、Explorer 重启、RDP 进出、用户切换 | 不重复播放、不挂死，不把断开与锁定混为同一状态 |
| 视频 | H.264、HEVC Main10 SDR、PQ、HLG、旋转 MOV、低帧率、239.76 FPS、损坏文件、明确不支持的 DV | 转码与播放均验收，原文件哈希不变，错误清楚 |
| 安装数据 | 默认目录、自选 D 盘、普通用户、只读目录、改目录升级、多份程序共存 | 设置/媒体保持权威，不操作其他安装的数据和进程 |

每条记录至少包含 Windows build、GPU / 驱动、显示时序、媒体工具版本、源与目标规格、实际路线、首帧耗时、循环情况、丢帧及资源占用。只将完成这些验收的组合标为“已验证”，其余写“理论支持 / 待验证”。
