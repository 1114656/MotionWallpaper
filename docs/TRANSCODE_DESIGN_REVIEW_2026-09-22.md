**MotionWallpaper 转码与播放设计审查 · 2026-09-22**

审查对象：`e8f9e23` 加当前工作区已有修复。覆盖导入、封面、源信息探测、转码、性能副本、显示状态、缓存、打包与测试。本轮只新增审查报告，未修改程序、驱动或用户设置。结论来自源码、当前电脑日志、厂商资料及独立日志轮转复现，不代表已经完成 NVIDIA / Intel / AMD 全部实机认证。

用户已确认产品优先级：**稳定、省电优先，允许性能副本转成 SDR，始终保留原文件。**

`.mov` 是容器，不能据此判断性能或兼容性。计划必须读取视频编码、profile、位深、色度采样、色彩标记、分辨率和帧率。输入能被转码工具读取、硬件能解码输入、硬件能编码输出、Windows 能播放输出，是四项不同能力。

**当前电脑的事实**

- `nvidia-smi` 报告 NVIDIA GeForce GTX 1070、驱动 **560.94**。
- 当前 FFmpeg 的 NVENC 实际报错为要求 API 13.0、驱动仅提供 API 12.2。NVIDIA SDK 13.0 的 Windows 驱动要求是 **570 或更高**；这是当前工具链的门槛，不是所有 FFmpeg 版本的永久门槛。[NVIDIA 官方要求](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/read-me/index.html)
- 上一轮真实视频转码在本地时间 **14:16:45** 成功，日志记录后端为 OpenH264 软件编码、硬件解码，目标 1080p60。随后已存在约 674 MiB 的完整兼容副本。这里只确认完整转码成功，不用它推断所有视频或显卡都已兼容。
- 审查时用户已关闭桌面播放、活动播放及屏保。此时 runtime 仍出现 `applying / pause-pending` 且 renderer PID 为 0，属于需要修正的状态表达，不能据此认定副本解码失败。

**优先修正的设计问题**

1. **P1：普通性能副本仍会被屏保切换取消。这是现有缺陷。**

   上一轮修复让 `cpu-smooth` 可以跨屏保继续，但普通 `balanced / power-saver` 只在 `DesktopPlay` 保持等待副本。进入屏保后切回源视频，转码被要求停止，临时文件被删除。默认 30 秒屏保与耗时数分钟的任务冲突；锁屏、熄屏也会丢弃本次进度。

   证据：[Agent.cpp:2732](../native/MotionWallpaper.Agent/Agent.cpp#L2732)、[Agent.cpp:2920](../native/MotionWallpaper.Agent/Agent.cpp#L2920)、[VideoOptimizer.cpp:1039](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L1039)、[VideoOptimizer.cpp:1817](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L1817)。

   建议：将任务生命周期与桌面/屏保呈现分离。接通电源时允许选定的生成任务在屏保或锁屏后继续，Renderer 仍遵守锁屏停止规则；这不需要阻止锁屏。电池/睡眠需要暂停时采用可验证分段检查点，或明确说明本次必须重做，不能把“取消后从头生成”伪装成续传。优先避免无必要的取消，不必第一步就实现复杂的任意位置续编码。

2. **P1：输出目标没有独立定义，是否转码只看尺寸和帧率。与新目标不符。**

   普通性能档只有尺寸或帧率需要降低才创建副本；编码不兼容只在内部 CPU 模式额外处理。1080p30 HEVC 等输入即使需要统一为 H.264，也可能被判定“无需优化”；显式生成请求可以直接完成而不产生新文件。

   证据：[VideoOptimizer.cpp:781](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L781)、[VideoOptimizer.cpp:916](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L916)、[VideoVariantPolicy.h:183](../native/MotionWallpaper.Agent/VideoVariantPolicy.h#L183)。

   建议：建立 `OutputSpec`，明确输出容器、codec、profile、位深、色彩、尺寸、帧率和码率。只有源文件已经满足目标规格并通过播放检查时，才直接复用源文件；不要单凭“无需降分辨率”就跳过转换。

3. **P1：HDR/高位深策略与“允许 SDR 副本”冲突；当前没有色调映射实现。**

   HDR/BT.2020 被禁止生成 CPU 兼容副本，缺少硬解时可能一直停留在静态预览。普通画质档对 SDR Main10 也偏向强制 HEVC Main10。与此同时，`softwareFallbackAllowed` 还决定输出 H.264 还是 HEVC，混合了画质政策和编码器能力。

   证据：[VideoVariantPolicy.h:10](../native/MotionWallpaper.Agent/VideoVariantPolicy.h#L10)、[VideoVariantPolicy.h:78](../native/MotionWallpaper.Agent/VideoVariantPolicy.h#L78)、[VideoTranscoder.h:26](../native/MotionWallpaper.Agent/VideoTranscoder.h#L26)、[VideoOptimizer.cpp:784](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L784)。

   当前滤镜只做帧率、缩放和像素格式转换。[VideoTranscoder.cpp:194](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L194) 的 `format=nv12/yuv420p` **不等于 HDR→SDR**。需要明确处理 PQ/HLG、线性化、亮度映射、色域转换及 BT.709 输出标记；SDR 10-bit 降到 8-bit 则需要正确量化/抖动。未知色彩信息不能当作已确认 SDR。

   输出验收也要从“与原片色彩标记相等”改成“满足目标规格”。当前 [VideoOptimizer.cpp:1765](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L1765) 的相等检查会拒绝有意生成的 SDR 输出。Renderer 当前是 8-bit BGRA，项目尚未实现完整原生 HDR 呈现，因此不宜把保留 HDR 编码标签当作端到端 HDR 已支持。

4. **P1：导入入口被系统源格式支持卡住，FFmpeg 没有成为完整兼容层。**

   导入先通过 Media Foundation SourceReader 检查源视频；失败直接拒绝，FFmpeg 回退只在封面阶段。FFmpeg 可以读取、但本机 MF 不能解析的 MOV 或其他视频没有机会生成 H.264 副本。缺 HEVC 扩展不一定导致 native 元数据读取失败，不能把所有 HEVC 导入失败都归因于扩展。

   证据：[MediaLibrary.cpp:411](../native/MotionWallpaper.App/MediaLibrary.cpp#L411)、[MediaLibrary.cpp:815](../native/MotionWallpaper.App/MediaLibrary.cpp#L815)、[ThumbnailGenerator.cpp:302](../native/MotionWallpaper.App/ThumbnailGenerator.cpp#L302)。

   建议随包提供固定版本 `ffprobe`，或直接使用 FFmpeg 库生成结构化输入信息。输入阶段依赖转码工具能读取，播放验收阶段才要求 Windows 能解码输出。仍保留尺寸、时长、文件权限和资源上限检查。[ffprobe 官方文档](https://ffmpeg.org/ffprobe.html)

5. **P1：缺少环境与输出的短片段预检，错误可能到整片完成才暴露。**

   硬件候选按厂商和显存排列，没有独立记录驱动、输入解码能力、输出编码能力和参数支持。打包时 `-encoders` 只说明编码器被编译进去。当前 NVENC 驱动不兼容缓存只在单个作业内有效。实际 MF 首帧与色彩校验发生在完整转码之后，可能花数分钟后删掉成品再试下一个后端。

   证据：[VideoTranscoder.cpp:695](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L695)、[VideoTranscoder.cpp:858](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L858)、[VideoTranscoder.cpp:968](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L968)、[prepare-ffmpeg.ps1:47](../scripts/prepare-ffmpeg.ps1#L47)。

   建议先做有总时间上限的 2–5 秒短片段转码和真实播放验收，再运行完整作业。设备结果按 GPU 标识、驱动版本、FFmpeg 版本、输入类别、输出规格及参数缓存。确定性不支持应快速跳过；设备忙等瞬态错误应有限重试。完整文件的最终验收继续保留。

6. **P1：部分系统解码检查没有墙钟超时。属于可靠性风险。**

   封面和输出首帧检查的同步 `ReadSample` 只有循环次数上限；一次调用卡在系统解码器内，次数上限不会生效。封面不返回时，后面的 FFmpeg 回退和转码请求也无法执行。这一阻塞风险由调用结构可见，本轮未人为复现驱动挂起。

   证据：[ThumbnailGenerator.cpp:241](../native/MotionWallpaper.App/ThumbnailGenerator.cpp#L241)、[MainWindow.xaml.cpp:2771](../native/MotionWallpaper.App/MainWindow.xaml.cpp#L2771)、[VideoTranscoder.cpp:774](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L774)。

   建议把不可信媒体探测/解码验收放入可终止的辅助进程，增加超时和取消；不能只给同步调用外包一个无法结束底层工作的等待超时。

7. **P2：转码路线与默认档位还不够省电。**

   硬件编码路径仍统一使用 GPU 下载、CPU Lanczos 缩放、再上传；封装脚本检查了 `scale_cuda`，实际路线没有使用它。对 4K 高帧率输入，这增加 CPU、内存带宽和同步开销。应按厂商实现经过验证的 GPU 缩放/色彩路线，保留现有路径作回退，不能仅凭 NVENC/QSV/AMF 名称认定整个流程已在 GPU 上运行。

   辅助软件编码的 GPU 解码器只选择第一个硬件候选；旧大显存独显失败后，不会遍历可能可用的新核显。解码器和编码器应分别探测与排序。[VideoTranscoder.cpp:194](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L194)、[VideoTranscoder.cpp:858](../native/MotionWallpaper.Agent/VideoTranscoder.cpp#L858)。

   当前平衡档最高 120 FPS、低功耗最高 60 FPS，更适合保流畅的策略，不能自然等同低功耗。建议默认平衡 60 FPS、节能 30 FPS，120 FPS 作为高刷新率设备的高级选项。逻辑 CPU 核数只适合初始估计，不能替代播放实测。[VideoVariantPolicy.h:109](../native/MotionWallpaper.Agent/VideoVariantPolicy.h#L109)、[PlaybackCapabilityPolicy.h:48](../native/MotionWallpaper.Agent/PlaybackCapabilityPolicy.h#L48)。

   显式软件模式还把 CPU 解码与 WARP 软件合成绑在一起。只需 CPU 解码的设备，仍可能使用物理 GPU 低成本合成，应拆开这两项选择。[Renderer.cpp:559](../native/MotionWallpaper.Renderer/Renderer.cpp#L559)。已有 auto 路径会对新副本重新选择解码设备，应保留，不能误认为一次 HEVC 失败会永久禁用 H.264 硬解。

   缓存还会按显示器精确尺寸/刷新率生成规格，但每个档位只保留一个副本。反复插拔高分辨率/高刷新率显示器、切换横竖屏，可能重新生成之前已有的规格。建议保留少量固定规格，按总配额淘汰，并复用符合目标预算的已有版本。[VariantCache.h:189](../native/MotionWallpaper.Common/VariantCache.h#L189)、[VideoOptimizer.cpp:653](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L653)。

8. **P2：热路径日志和停止状态存在真实问题。**

   每次缓存命中都追加日志，现场同一秒多次重复“使用壁纸优化缓存”。轮转又在旧日志的 `ifstream` 仍开启时替换该文件，Windows 上可被自己的读句柄阻止。工作区独立 C++ 样本已复现：读句柄开启时替换失败，错误码 5；关闭后成功。生产日志在本轮已超过设置的 2 MiB 门槛。

   证据：[VideoOptimizer.cpp:793](../native/MotionWallpaper.Agent/VideoOptimizer.cpp#L793)、[Common.cpp:545](../native/MotionWallpaper.Common/Common.cpp#L545)。复现材料位于 `<本地验证目录>\motion-audit-logrotate`，未改生产日志。建议只记录首次采用/路径变化，轮转前关闭句柄并检查替换结果。

   `Stopped` 后无 renderer 的路线仍可能显示 `pause-pending`。建议先依据期望状态表达“已停止/已暂停”，再对确实需要 renderer ACK 的转换显示等待。[Agent.cpp:1190](../native/MotionWallpaper.Agent/Agent.cpp#L1190)、[Agent.cpp:2967](../native/MotionWallpaper.Agent/Agent.cpp#L2967)。缓存访问时间已有约 30 分钟节流，不应误报为每轮都写时间；清理也已有节流。

9. **P2：自动测试与显卡支持承诺之间缺少实机验收层。**

   上一轮 81 项测试通过，说明这些检查通过；真实压缩视频样本主要是 64×64@30 H.264，名为真实 Renderer 首帧的测试使用 BMP，不能据此声明 4K240 HEVC Main10、HDR 转 SDR 或三家显卡驱动已覆盖。[Tests.cpp:2239](../native/MotionWallpaper.Tests/Tests.cpp#L2239)、[Tests.cpp:2894](../native/MotionWallpaper.Tests/Tests.cpp#L2894)。

   建议增加有固定输入/预期输出的媒体集成测试和专用 GPU 测试机。旧卡不支持某参数时改用普通 VBR 的兼容档，也应实机验证；本轮没有 Intel/AMD 上的失败复现，不能把这种兼容性风险写成已确认故障。

**建议的支持范围与环境要求**

最低支持应由可用能力与实测共同决定；型号只用于预筛。下表是拟定的主要测试基线，不是已完成认证，也不是厂商 API 的最低硬件规格。

| 路线 | 建议主要测试设备 | 驱动与验收要求 |
| --- | --- | --- |
| NVIDIA | RTX 2060 及后续带 NVENC 的型号；另保留 GTX 1070 作为老卡兼容样本 | 当前发行工具链要求驱动 570+ 才启用 NVENC；需通过 H.264 短片段转码和播放测试 |
| Intel | 第 11 代或后续具备 Xe 核显的 Core、Arc；核显必须启用 | 固定经验证的 Intel/OEM 驱动版本和 VPL 运行时；通过 QSV 端到端测试，不能只看 CPU 代数 |
| AMD | RX 6600、RX 7600 等具备对应 VCN 编解码能力的型号 | 固定经验证的 Adrenalin/OEM 驱动版本；通过 AMF 端到端测试，不能泛称“RX 6000 系列都支持” |
| 兼容模式 | 未进入主测试范围，但能完成 H.264 播放的设备 | 允许 CPU 转码、物理 GPU 合成，默认 720p/1080p30；明确显示首次转换较慢 |

NVIDIA 官方矩阵确认 GTX 1070 本身具有 H.264/HEVC 编码能力，因此本机这次首先是驱动与工具链不匹配，不能简单归因于“显卡太老”。[NVIDIA 能力矩阵](https://developer.nvidia.com/video-encode-decode-support-matrix)

Intel 当前 VPL 主支持路线包含第 11 代及后续带核显的 Core 和 Arc。[Intel VPL 支持说明](https://www.intel.com/content/www/us/en/developer/tools/vpl/overview.html) AMD 官方矩阵将 RX 63xx/64xx/65xx 的 Navi24 标为仅解码，RX 66xx 等不能与它们混为一谈。[AMD 能力矩阵](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/GPU-and-APU-HW-Features-and-Support)

Intel/AMD 的确切最低驱动目前尚无本项目实机证据，不应编造统一数字。发布时应同时固定工具链和通过测试的精确驱动版本；“总是安装最新版”也不能代替兼容表。探针通过才启用相应硬件路线。

建议主要支持系统先定为 Windows 11 x64、可用的物理 D3D11 设备，内存建议 8 GB 起、16 GB 更合适；这属于项目拟定支持范围，仍需测试。Windows 10 可保留单独兼容范围。正常用户不需要安装 Visual Studio、CUDA Toolkit 或转码 SDK，程序随包提供所需工具；驱动由系统正常安装。

输入先重点验收 4K、最高约 240 FPS 的 H.264/HEVC Main/Main10 MOV/MP4；其余 FFmpeg 可读格式通过探测后进入兼容模式。ProRes、AV1、4:2:2/4:4:4、8K 不以扩展名或“新显卡”推断支持，分别报告解码路线和预计耗时。硬解源 4K240 的吞吐与能播放最终 1080p30 是不同要求。

**建议的主流程**

```mermaid
flowchart LR
    A[保留原文件] --> B[独立探测编码和色彩]
    B --> C[确定 H.264 SDR 输出规格]
    C --> D[选择已验证的解码和编码路线]
    D --> E[短片段转码与播放预检]
    E --> F[独立后台任务生成完整副本]
    F --> G[完整验收和原子发布]
    G --> H[Renderer 首帧确认后切换]
```

默认输出采用 MP4/H.264 High、8-bit 4:2:0 SDR，去音轨，色彩标记与实际变换一致。节能档默认 1080p30，平衡档默认 1080p60；不放大低分辨率源、不对低帧率源插帧，29.97/59.94 等时间基应有明确策略。2K/4K60 作为通过设备测试后的高清选项；120 FPS 单独开启。不同路线可生成相同输出规格，不应靠编码器名字决定画质政策。

原文件已满足目标且经过实际播放检查时可以直接使用，避免无意义重编码。输出尺寸仍需按宽高比与显示需要选择，多显示器继续利用已有共享解码能力。

副本缓存键包含源身份、输出规格和转换算法版本。GPU/驱动/FFmpeg 版本属于能力缓存，通常不应因为更换驱动就删除仍然有效的媒体副本。长任务需记录当前阶段、百分比、速度、预计剩余时间、实际路线和明确失败原因；ETA 应在速度稳定后显示为估计值。

**实施顺序与验收**

| 顺序 | 改动 | 验收重点 |
| --- | --- | --- |
| 1 | 将所有性能副本任务与屏保/锁屏呈现分离；修停止状态、重复日志及轮转 | 30 秒屏保、手动锁屏、关闭设置窗口都不无故重置任务；用户安全锁照常生效 |
| 2 | 独立 SourceProbe 与 OutputSpec，导入增加 FFmpeg 探测，探针统一超时/取消 | MF 不认识但 FFmpeg 可读的输入可以转 H.264；不能播放的输出在整片转换前被发现 |
| 3 | 实现并验证 SDR 目标及 HDR→SDR 色彩转换 | PQ/HLG、10-bit SDR、BT.2020 测试素材亮度和色彩正确，源文件哈希不变 |
| 4 | 加能力缓存、发行版驱动要求、参数兼容档，再优化 GPU 内存路径 | 本机 560.94 快速进入兼容路线；合格驱动启用硬编；Intel/AMD 各自通过真实媒体测试 |

测试矩阵至少覆盖：H.264 SDR、HEVC Main10 SDR、PQ、HLG、缺失/异常色彩标记、MOV/MP4、240/239.76 FPS、损坏文件、驱动缺失/不兼容、无硬编、双显卡、不同刷新率双屏，以及转码期间屏保/锁屏/电池/取消/退出。检查完整时长、输出规格、首帧/循环/切换、实际资源消耗，保留原文件的哈希校验。

已有值得保留的基础：源文件不改写、固定哈希的 FFmpeg 包、GPU LUID 绑定、同卡同源共享解码、临时文件隔离、输出验收与原子发布、播放文件租约、FFmpeg 停滞超时、首帧确认再交接。应在这些基础上统一策略，而不是继续给不同异常分别叠加分支。
