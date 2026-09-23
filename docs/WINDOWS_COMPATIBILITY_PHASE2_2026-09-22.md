# Windows 兼容性第二批修复

日期：2026-09-22。承接第一批修复，本批处理不同刷新率屏幕的呈现调度，以及 CPU 解码和显示设备分离。

## 实现

- 屏幕拓扑通过 QueryDisplayConfig 获取约分后的有理刷新率，优先使用物理信号时序，避免把 Windows 11 DRR 的虚拟刷新率当成屏幕时序。Windows 10 不支持新查询标志时回退；拓扑变化期间有界重试；查不到时保留既有整数刷新率回退。
- Renderer 按媒体、GPU 和刷新率分组。同 GPU 同刷新率仍共享解码；60/144 Hz、60000/1001 与 120000/1001 等组合分开进程调度。代价是不同刷新率的屏幕可能增加解码实例；没有宣称已实现跨进程共享解码帧。
- 每个输出独立维护交换链就绪句柄、待呈现帧和进度。使用创建时的 FRAME_LATENCY_WAITABLE_OBJECT 标志、最大队列延迟 1、零超时就绪检查和 Present1(DO_NOT_WAIT)。队列忙时保留待提交帧，下次重试，不同步等待另一屏的 VSync。所有输出均提交首帧后才确认启动。
- 冻结使用离屏纹理捕获并提交 DirectComposition 静态表面，不依赖显示器继续提供交换链就绪信号。每次目标命令重置冻结捕获阶段，覆盖相邻冻结命令；全部输出捕获成功后才确认。恢复播放时保留静态表面到交接完成，交换链释放时先关闭就绪句柄。
- 显式软件模式不再强制 WARP。先创建物理 GPU 显示设备；全部失败时才回退 WARP。CPU 解码的 Media Engine 不传 DXGI manager，使用 WIC BGRA 位图接收帧后上传显示纹理，复用相同尺寸的位图。缩放和视频处理仍可能在 CPU 完成，不能把这条路径称为全 GPU 处理。
- GPU 负载探测路由、播放路由标识、每屏状态归属及热插拔重建均使用相同刷新率分组。

节能、平衡、原画三档规则保持不变；副本目标仍沿用已有整数 Hz 规则。此次有理数时序用于显示路由分组，不是把转码帧率算法改成完整有理数计算。

## 验证范围

本机只读拓扑查询返回一个可见 60/1 Hz 屏幕。60+144、59.94+119.88、约分等价、刷新率变化和热插拔路由由自动夹具覆盖；不等于已完成真实混刷多屏扫描时序验收。

独立 Media Engine 探针证实：无 DXGI manager 的 CPU 路径直接向物理 GPU 纹理 TransferVideoFrame 返回 E_NOINTERFACE，向 IWICBitmap 返回 S_OK。新上传测试检查带行跨度的 BGRA 像素，并拒绝尺寸或用途错误的目标纹理。

真实硬件交换链子进程验证创建标志、最大延迟、首帧就绪检查、非阻塞 Present1 及句柄关闭。没有物理 D3D11 的测试环境明确跳过该项，其他 API 失败不跳过。本机实际创建成功；无目标窗口时返回 OCCLUDED，该结果只代表调用契约通过，不代表物理屏幕已经扫描显示。

真实隐藏 Renderer 使用 H.264 片段验证 CPU/WIC/物理 GPU 和自动 DXGI 两条路径，两份输出共同确认首帧、冻结、恢复及连续冻结。测试只建立隐藏渲染窗口，不改变用户显示模式或壁纸设置。

最终 `scripts/build-native.ps1` 完成，110 项原生测试通过，编译 0 警告、0 错误；本机交换链检查未跳过。用户视频既有 4 秒 1080p60 副本分别以软件和自动模式运行 6 秒，再冻结、恢复并正常退出。两条路径均返回完整目标 ACK；软件路径日志为 `pipeline decode cpu presentation physical-gpu transfer wic-upload`。短片副本 SHA-256 前后均为 `55FE6CC188DAB9B7D8880EDAE24B9ECD04E36C333DDE52AF508328FED0914A7B`，记录保存在 `<本地验证目录>\motion-phase2-user-video-renderer.log`。这不是完整 525 秒原视频的长时性能认证。

## 本机交付

第二批安装包为 `artifacts/MotionWallpaper-v0.2.0-alpha.4-setup-windows-x64.exe`，66,243,330 字节，SHA-256 为 `4bd52e3f252ad48aa707cbb8f68d2c014a7df115fc98540f75d8651205b2f7e0`。本次重新编译并校验安装包；安装器逻辑未再改动，未重复第一批安装/卸载冒烟测试。

已更新 `<安装目录>\App`，App、Agent、Renderer 三个文件均与验证后的发布文件哈希一致。App 与 Agent 已重新启动且响应正常；用户关闭的桌面播放、活动播放和屏保保持关闭，runtime 为 `paused / playback-stopped`。原安装文件备份在 `<本地验证目录>\motion-v6-backup-20260922-201335`。

部署前后配置 SHA-256 均为 `428A795E7C87E8D144B8DAAEDD74DEB8FA23E0BCCC164E3DBD633789914D8443`；安装目录中完整源 MOV 保持 `444CC93855AAE60C5D416A021B0D8C8DE5C83600B0C61EA46A0A30A5F5D24C8E`。本批未提交或推送到 GitHub。

## 实机边界

尚须在 Intel、AMD、多 GPU、不同驱动、Windows 10/N 及真实混刷屏幕上验收。DO_NOT_WAIT 避免交换链排队等待；不保证驱动、帧传输或 DirectComposition 提交函数在设备异常时永不阻塞。既有一次性交接仍使用 WaitForCommitCompletion。

长期固定 FFmpeg 制品存储、特殊 Dolby Vision 输入、跨进程断点续编码和完整硬件矩阵不属于本批新增能力。

## 官方依据

- [显示路径物理与虚拟刷新率](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_info)
- [QueryDisplayConfig 标志与拓扑变化处理](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig)
- [帧延迟就绪句柄的等待与关闭](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
- [非阻塞呈现及重试结果](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)
- [Media Engine DXGI manager 与软件解码行为](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-media-engine-dxgi-manager)
- [TransferVideoFrame 支持的目标](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengine-transfervideoframe)
