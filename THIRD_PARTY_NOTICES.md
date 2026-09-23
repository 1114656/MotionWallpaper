# 第三方软件声明

MotionWallpaper 的 MIT License 仅适用于本仓库的原创源代码。第三方组件继续适用各自的许可证条款。

## FFmpeg

性能副本后端调用未经修改的 FFmpeg 可执行文件；系统缺少解码器时，原画播放器动态加载同目录的 FFmpeg 共享库作为备用解码器。二者均来源于固定版本的 BtbN Windows x64 LGPL shared build，按照 GNU LGPL v3 授权。二进制负载会在 `Tools/ffmpeg` 中保留对应许可证和声明，不向 Windows 注册系统解码器。

- 项目主页：https://ffmpeg.org/
- 构建分发：https://github.com/BtbN/FFmpeg-Builds
- 仓库内声明：`third_party/FFmpeg-NOTICE.txt`

## OpenH264

固定的 BtbN FFmpeg 构建启用了 Cisco OpenH264 编码器。OpenH264 源代码采用 BSD 许可证；发布负载在 `Tools/ffmpeg/LICENSE-OpenH264.txt` 中保留其版权、条件和免责声明。

- 项目主页：https://github.com/cisco/openh264
- 仓库内许可证副本：`third_party/OpenH264-LICENSE.txt`

这里使用的是第三方 FFmpeg 构建中集成的 OpenH264，不是从 Cisco 官方下载的预编译 OpenH264 二进制；本声明不主张 Cisco 对官方预编译二进制提供的专利许可适用于该构建。正式分发前，发布者仍需独立确认适用地区的 H.264 专利许可要求。

## Microsoft Windows 组件

项目通过 NuGet 恢复以下组件：

- Microsoft.WindowsAppSDK.Foundation 1.8.260803002；
- Microsoft.WindowsAppSDK.InteractiveExperiences 1.8.260708001；
- Microsoft.WindowsAppSDK.WinUI 1.8.260803003；
- Microsoft.Windows.CppWinRT 3.0.260715.1；
- Microsoft.Windows.SDK.BuildTools 10.0.28000.2526。

Windows App SDK 组件适用其 NuGet 包中附带的 Microsoft 许可证；C++/WinRT 使用 MIT License；SDK Build Tools 仅用于构建。

发布负载还会从用于构建的 Visual Studio 2026 MSVC v145 Redistributable 目录旁加载部署 x64 Visual C++ Runtime DLL。其使用和再分发受对应 Visual Studio 许可条款约束；这些 DLL 只用于免管理员安装和便携运行，不替换或修改系统组件。

项目已不再引用 Windows App SDK 2.x Engineering Preview。固定使用的 Windows App SDK 1.8 组件包含可分发代码，其分发权仍以包内 Microsoft 许可证为准。发布时必须保留这些许可证文件，并完成 `docs/RELEASING.md` 中的检查。
