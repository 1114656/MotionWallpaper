#pragma once
#include "BuiltinVideo.h"
#include <d3d10_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <thread>

namespace motion::renderer {
    // Runs only in a bounded, job-owned child. A profile bit in a driver is
    // insufficient: exercise real D3D11VA frames and both presentation shaders.
    inline int probe_builtin_playback(std::wstring const& path, unsigned fps) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
        for (UINT index = 0; index < 8; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
            DXGI_ADAPTER_DESC1 adapterDesc{};
            if (FAILED(adapter->GetDesc1(&adapterDesc)) || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) continue;
            ComPtr<ID3D10Multithread> protection;
            if (FAILED(device.As(&protection))) continue;
            protection->SetMultithreadProtected(TRUE);
            BuiltinVideo video;
            if (!video.Start(path, device.Get(), true, false, fps)) continue;
            video.Play();
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            LONGLONG previous{-1}; unsigned frames{};
            ComPtr<ID3D11Texture2D> sdr, linear;
            while (std::chrono::steady_clock::now() < deadline) {
                LONGLONG timestamp{};
                auto status = video.Tick(&timestamp);
                if (FAILED(status)) break;
                if (status != S_OK || timestamp == previous) { Sleep(2); continue; }
                previous = timestamp;
                if (!sdr) {
                    DWORD width{}, height{};
                    if (FAILED(video.Size(&width, &height)) || !width || !height) break;
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = width; desc.Height = height;
                    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (FAILED(device->CreateTexture2D(&desc, nullptr, &sdr))) break;
                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    if (FAILED(device->CreateTexture2D(&desc, nullptr, &linear))) break;
                }
                if (!video.Hardware() || FAILED(video.Draw(sdr.Get(), {0,0,1,1})) ||
                    FAILED(video.Draw(linear.Get(), {0,0,1,1}))) break;
                if (++frames >= (std::min)(12u, (std::max)(1u, fps))) return 0;
            }
        }
        return 2;
    }
}
