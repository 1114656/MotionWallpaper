# 面向大量用户的架构审阅：兼容性、内存和画质流畅度

审阅日期：2026-09-22。基于当前工作区中已经完成第一、第二批修复的代码；不是只审阅远端旧版本。本轮只读审阅代码并运行隔离的隐藏 Renderer 测量，未改业务代码、用户设置或显示模式。

结论：保留 App / Agent / Renderer 分离、原文件与性能副本分离、逐 GPU 探测隔离和混刷屏幕分组的基础架构。面向大量用户推广前，优先完善运行中故障恢复、路由退役、帧时序和副本几何，再建立分状态资源预算及发布验收。110 项已有测试是功能回归基础，不能代表所有 Windows / GPU / 驱动组合已经稳定、低占用和流畅。

## 当前实测及其限制

当前未播放、设置窗口已关闭时，Agent 工作集约 18.8 MiB，Private Bytes 约 2.8 MiB。说明常驻策略进程不是这次发现的主要占用来源。

隔离启动新 Renderer：GTX 1070、单个隐藏的 3840×2160 输出，播放用户视频已经验收的 1920×1080 H.264 SDR 60 FPS、4 秒短片副本。播放 5 秒后冻结，冻结 35 秒后正常退出。没有模拟系统低内存或改变用户壁纸。

| 路径 | 播放工作集 MiB | 冻结 35 秒工作集 MiB | 播放 Private Bytes MiB | 冻结 35 秒 Private Bytes MiB |
| --- | ---: | ---: | ---: | ---: |
| 自动 DXGI 路径 | 79.0 | 79.8 | 101.0 | 131.1 |
| CPU 解码、物理 GPU 显示 | 187.9 | 192.2 | 186.8 | 220.1 |

Private Bytes 是私有提交量，不等于当前驻留物理 RAM，也不能和工作集直接相加。此表不包含独立显存或 DWM 内存，隐藏窗口测试不能用来认证物理显示帧率。冻结后 CPU 时间基本停止增长，但没有记录 decoder released / residency compact；与代码中的保留策略相符。它不证明内存泄漏，也不是长时间稳定性测试。

原始数据：`<本地验证目录>\motion-architecture-residency-20260922.json`；测量脚本：`<本地验证目录>\audit-motion-residency.ps1`。

## 优先问题

### 1. P1：一屏失败可能使其他屏旧渲染进程持续积累

[Agent.cpp:981](../native/MotionWallpaper.Agent/Agent.cpp#L981) 汇总所有新路由的 `allReady`；985–1003 仅在全部成功时 Stop 旧路由，否则只 ReapExited。675–677 的 Refresh 不会停止仍活着的旧进程。951–952 又会为新媒体创建新 Renderer。

触发条件：两个独立路由，一屏在播放现有平衡/节能副本时持续启动失败，另一屏连续切换有效副本 A→B→C。旧健康进程的解码器、显存和媒体租约可能随切换累积。原画失败转静态海报是另一个分支，不能替代这里的普遍生命周期保证。

这是控制流确认的退役缺口，尚未注入真实显卡失败测量累积量。建议按显示目标独立完成交接，每个目标最多保留一个旧画面与一个候选，取消被更新请求替代的候选，并设置退役截止时间。回归必须包含“一屏一直失败，另一屏反复切换”。

### 2. P1：首帧之后缺少播放健康检查

[Agent.cpp:334](../native/MotionWallpaper.Agent/Agent.cpp#L334) 的超时仅覆盖目标 ACK 尚未确认；376 的 TargetReady 使用已确认 revision，675 的 Refresh 检查进程退出。[Renderer.cpp:968](../native/MotionWallpaper.Renderer/Renderer.cpp#L968) 对持续 NoFrame 只重新安排下一次尝试。

因此首帧后解码/驱动不再推进而进程仍活着时，界面可能保持“已应用”，没有持续帧进度来触发恢复。真实驱动发生概率未测，代码没有这类检测则是确定事实。

建议由 Renderer 报告消息循环心跳及每个输出的实际帧序号，Agent 独立监测；只在应播放时检查，正确排除暂停、冻结和低帧率素材。恢复按时间窗口限制次数，反复失败保留静态画面并显示原因，避免无限重启。用“ACK 后存活但不再推进”注入测试验证。

### 3. P1：软件 60 FPS 调度会叠加处理耗时

[SoftwareFramePolicy.h:13](../native/MotionWallpaper.Renderer/SoftwareFramePolicy.h#L13) 将 60 FPS 取整为 17 ms；[Renderer.cpp:884](../native/MotionWallpaper.Renderer/Renderer.cpp#L884) 将其作为最小等待间隔；1016 在处理完成后才重新启动定时器；[FrameScheduler.h:63](../native/MotionWallpaper.Renderer/FrameScheduler.h#L63) 设置相对单次期限。

连续帧间隔因此是“帧处理耗时＋17 ms＋消息调度延迟”。忽略处理耗时上限也只有约 58.82 FPS；若处理耗时为 3 ms，理论上限约 50 FPS。这是代码推导，不是本机显示 FPS 测量。当前测试验证了 60→17 的映射，未验证持续呈现节拍。

建议用 QPC 单调时钟和精确帧周期维护下一次目标时间，只等待剩余时间；与输出交换链就绪信号结合，避免累积延迟，错过时跳到下一期限而非突发追帧。保留 59.94 等有理时序。[微软相对定时器语义](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex)

### 4. P1：副本缩放与铺满显示的几何策略不一致

[VideoVariantPolicy.h:196](../native/MotionWallpaper.Agent/VideoVariantPolicy.h#L196) 将整个源图按适应尺寸缩入显示边界，Renderer 的 [CoverSource:540](../native/MotionWallpaper.Renderer/Renderer.cpp#L540) 却裁切并铺满。

3840×2160 源视频用于 3440×1440 屏幕时，副本仅为 2560×1440，播放时再放大约 1.344 倍。用于 1080×1920 竖屏时，副本约 1080×608，实际可见裁剪区域约 342×608，又放大到整屏。源素材本可提供足够像素，损失来自两阶段策略冲突。Tests.cpp 的既有竖屏用例固定了 1080×608，而没有验证最终可见区域清晰度。

建议将“填充/适应”作为一致的几何契约：填充时先按目标比例裁剪再缩放，或保留足够覆盖目标的中间尺寸；适应时允许留边，避免二次放大。副本缓存键需包含影响内容的裁切比例和策略。CPU 兼容降清晰度应明确区别于平衡档目标。

### 5. P2：长期冻结仍保留完整播放资源，缺少显存预算

[ResidencyPolicy.h:15](../native/MotionWallpaper.Renderer/ResidencyPolicy.h#L15) 仅在系统 RAM low-memory 时允许 Compact，[Renderer.cpp:1144](../native/MotionWallpaper.Renderer/Renderer.cpp#L1144) 按该策略释放；正常暂停保留解码器也是现有测试要求的行为。这是快速恢复与低驻留占用之间的设计取舍。

一张 4K BGRA8 图像约 31.64 MiB，冻结而未压缩时每屏两张交换链加一张冻结图约 94.92 MiB 的逻辑像素容量；CPU 路径每种输出尺寸另有一张 WIC 位图，尚未计算解码和驱动缓存。该估算不是实际物理显存；Renderer.cpp:447 的内部估算本身还漏算交换链与冻结图同时存在的情形。

此外 [SoftwareVideoTransfer.h:23](../native/MotionWallpaper.Renderer/SoftwareVideoTransfer.h#L23) 按显示纹理尺寸创建位图：即使副本是 1080p，4K 屏仍在 CPU 路径产生完整 4K BGRA 并上传。若达到 60 次/秒，仅这部分原始像素上传约为 1.99 GB/s；这是带宽量级估算，不是实测吞吐或全部内存流量。

建议分短暂停保热、长期冻结收缩、不可见/锁屏释放三个状态；让超时、RAM 和 DXGI 显存预算共同决定收缩。CPU 桥接优先上传与有效源像素相称的纹理，再用 GPU 完成显示缩放，避免先在 CPU 放大。仍需按实际驱动验收质量及带宽收益。微软明确指出超出进程显存预算可能因换页导致卡顿：[QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)。

### 6. P2：大壁纸库缺少有效 UI 虚拟化

[MainWindow.xaml:217](../native/MotionWallpaper.App/MainWindow.xaml#L217) 外层 ScrollViewer/纵向 StackPanel 给集合无限布局空间；270 附近 GridView 只有 MinHeight。[MainWindow.xaml.cpp:1775](../native/MotionWallpaper.App/MainWindow.xaml.cpp#L1775) 主动创建所有 GridViewItem、布局和 BitmapImage，而非只创建可见项目。封面已有 480×270 上限，不是全部读取 4K 源图的问题。

几百到几千张壁纸时，刷新和全库搜索会构造大量主线程对象；具体内存增量需专门压测。建议有限高度 Grid 承载单一滚动视口，使用 ItemsSource / DataTemplate、增量查询和可见项缩略图加载回收。[WinUI 集合虚拟化约束](https://learn.microsoft.com/en-ca/windows/apps/develop/performance/optimize-gridview-and-listview)

## 需要重新讨论的产品规则

“节能＝最高 1080p、显示器刷新率；平衡＝显示器分辨率、刷新率＋30；原画直出”是用户确认的要求，目前不应静默更改。

从清晰、省电、流畅的目标看，建议重新评估平衡档的＋30。源帧率足够时，60 Hz 屏对应 90 FPS 副本，每秒待解码帧数比 60 FPS 多 50%，但同步显示不能显示 90 个完整新画面；非整数 90→60 采样还可能造成运动间隔不均。50% 是帧数差，不代表整机功耗必然增加 50%。[Present1 同步规则](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-present1)

建议默认匹配屏幕实际刷新率并受源帧率约束，＋30 保留为高级偏好候选，以实际帧间隔和功耗比较作决定。原画继续不自动转码，失败展示明确原因和用户可选的副本恢复入口。为了降低后台开销而偷偷修改用户画质档位也不合适。

## 面向公开发布的门槛

1. **长期可重建。** [prepare-ffmpeg.ps1:13](../scripts/prepare-ffmpeg.ps1#L13) 固定短期日构建，空缓存的测试和发布依赖下载成功；[上游只保留有限数量日构建](https://github.com/BtbN/FFmpeg-Builds#release-retention-policy)。需把验证过的归档保存至项目控制的长期制品位置，绑定哈希、版本和许可证，增加空缓存重建检查。已有用户的本地 FFmpeg 不会因为上游删除归档就突然失效。
2. **发布受验收约束。** 当前 CI/Release 仅一个 hosted runner，未接入现有视频素材回归及真实硬件验收结果。正式版本应关联最低 Windows、Windows 11、N 版、Intel/AMD/NVIDIA、混合显卡和双屏混刷记录；ARM64 保持实验性说明，不能凭 x64 模拟启动成功扩大承诺。
3. **签名与完整启动链。** 当前自有 App、Agent、Renderer 和安装器实测 NotSigned。面向大量用户应建立稳定发布者签名和时间戳，签名后生成哈希，并验证启用 Smart App Control 的干净系统。签名不保证绝不出现信誉提示。[微软签名与应用信誉说明](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
4. **长期播放指标。** 将 SDR/PQ/HLG/旋转素材回归接入发布，并补 30–60 分钟播放、循环、反复切换及长期冻结的资源曲线。记录进程 Private Bytes/工作集、GPU 预算和占用、实际呈现率、p95/p99 帧间隔、掉帧及恢复次数。先接入 [Media Engine 官方统计](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/ne-mfmediaengine-mf_media_engine_statistic)，再通过显示事件工具交叉验证，不能只看文件标称 FPS 或首帧 ACK。

不建议现在给所有设备承诺统一的“总内存低于 50 MB”。4K 双缓冲、解码表面和驱动均有实际成本。预算应区分 Agent、UI、Renderer、转码任务，以及 RAM、显存；分别定义空闲、播放、冻结和后台转码的验收条件。

建议实施顺序：先修路由退役与播放健康检查；再修帧期限调度和副本几何；随后做长期冻结收缩、CPU 显示上传预算及大库虚拟化；最后由签名、稳定依赖和实机性能矩阵约束正式发布。不同刷新率分进程是当前兼容性取舍，先把实例数控制有界，再决定跨刷新率共享解码是否值得增加复杂度。
