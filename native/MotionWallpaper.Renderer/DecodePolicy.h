#pragma once

#include <string_view>

namespace motion::renderer
{
    enum class DecodePath { Automatic, Hardware, Software };

    [[nodiscard]] constexpr DecodePath select_decode_path(std::wstring_view requestedMode) noexcept
    {
        if (requestedMode == L"software") return DecodePath::Software;
        if (requestedMode == L"hardware") return DecodePath::Hardware;
        return DecodePath::Automatic;
    }

    [[nodiscard]] constexpr bool allows_software_device_fallback(DecodePath path) noexcept
    {
        // Decode choice and presentation device are independent. Software
        // decode still attempts physical-GPU presentation before using WARP.
        return path != DecodePath::Hardware;
    }

    [[nodiscard]] constexpr bool uses_media_engine_dxgi_manager(
        DecodePath path, bool softwareDecodeFallback) noexcept
    {
        // In frame-server mode the DXGI manager enables both hardware decode
        // and video processing. Omitting it is the documented CPU contract;
        // merely omitting D3D11_CREATE_DEVICE_VIDEO_SUPPORT is insufficient.
        return path != DecodePath::Software && !softwareDecodeFallback;
    }
}
