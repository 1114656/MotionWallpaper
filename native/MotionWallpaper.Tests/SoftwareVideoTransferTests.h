#pragma once

#include "../MotionWallpaper.Renderer/DecodePolicy.h"
#include "../MotionWallpaper.Renderer/SoftwareVideoTransfer.h"

#include <array>
#include <cstring>

namespace motion::tests
{
    template <typename Require>
    void cpu_decode_and_presentation_device_are_independent(Require require)
    {
        using namespace motion::renderer;
        require(!uses_media_engine_dxgi_manager(DecodePath::Software, false) &&
            !uses_media_engine_dxgi_manager(DecodePath::Software, true),
            "physical presentation re-enabled hardware decoding for a CPU request");
        require(uses_media_engine_dxgi_manager(DecodePath::Hardware, false) &&
            !allows_software_device_fallback(DecodePath::Hardware),
            "strict hardware mode accepted CPU/WARP fallback");
        require(uses_media_engine_dxgi_manager(DecodePath::Automatic, false) &&
            !uses_media_engine_dxgi_manager(DecodePath::Automatic, true),
            "automatic device fallback did not switch the actual decoder contract");
        require(allows_software_device_fallback(DecodePath::Software) &&
            allows_software_device_fallback(DecodePath::Automatic),
            "CPU or automatic mode lost its last-resort presentation device");
    }

    template <typename Require>
    void software_bitmap_upload_preserves_pixels_and_rejects_mismatched_targets(Require require)
    {
        using Microsoft::WRL::ComPtr;
        using motion::renderer::SoftwareVideoTransfer;
        // WARP makes this copy regression deterministic on machines without a
        // physical GPU. Real Renderer smoke tests cover physical presentation.
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context)), "cannot create test presentation device");
        ComPtr<IWICImagingFactory> factory;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "cannot create WIC test factory");
        // Input rows have padding and deliberately distinct colors/alpha.
        std::array<BYTE, 32> pixels{
            10,20,30,255, 40,50,60,255, 70,80,90,255, 0xcc,0xcc,0xcc,0xcc,
            90,80,70,255, 60,50,40,255, 30,20,10,255, 0xdd,0xdd,0xdd,0xdd
        };
        ComPtr<IWICBitmap> bitmap;
        require(SUCCEEDED(factory->CreateBitmapFromMemory(3, 2, GUID_WICPixelFormat32bppBGRA,
            16, static_cast<UINT>(pixels.size()), pixels.data(), &bitmap)), "cannot create padded test bitmap");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 3;
        description.Height = 2;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &target)), "cannot create test target");
        require(SUCCEEDED(SoftwareVideoTransfer::UploadBitmap(context.Get(), target.Get(), bitmap.Get())),
            "CPU bitmap could not be uploaded to the independent presentation device");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &readback)), "cannot create readback");
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "cannot map readback");
        bool samePixels = std::memcmp(mapped.pData, pixels.data(), 12) == 0 &&
            std::memcmp(static_cast<BYTE*>(mapped.pData) + mapped.RowPitch, pixels.data() + 16, 12) == 0;
        context->Unmap(readback.Get(), 0);
        require(samePixels, "software upload corrupted BGRA pixels or row padding");
        require(SoftwareVideoTransfer::UploadBitmap(context.Get(), readback.Get(), bitmap.Get()) == E_INVALIDARG,
            "software upload accepted a staging texture as a presentation target");
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        description.CPUAccessFlags = 0;
        description.Width = 4;
        ComPtr<ID3D11Texture2D> wrongSize;
        require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &wrongSize)), "cannot create wrong-size test target");
        require(SoftwareVideoTransfer::UploadBitmap(context.Get(), wrongSize.Get(), bitmap.Get()) == E_INVALIDARG,
            "software upload accepted a target of different dimensions");
    }
}
