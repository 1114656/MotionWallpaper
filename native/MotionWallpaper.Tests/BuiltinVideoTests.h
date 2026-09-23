#pragma once
#include "../MotionWallpaper.Renderer/BuiltinVideo.h"
#include <d3d10_1.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace motion::tests {
    struct BuiltinVideoFixture {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        motion::renderer::BuiltinVideo video;
        static void Check(bool value, char const* text) {if(!value)throw std::runtime_error(text);}
        explicit BuiltinVideoFixture(bool hardware) {
            auto flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT | (hardware?D3D11_CREATE_DEVICE_VIDEO_SUPPORT:0);
            Check(SUCCEEDED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,
                nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"builtin test device unavailable");
            Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
            Check(SUCCEEDED(device.As(&multithread)),"builtin device has no multithread protection");
            multithread->SetMultithreadProtected(TRUE);
        }
        LONGLONG WaitFrame(LONGLONG minimum=0) {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            while(std::chrono::steady_clock::now()<deadline) {
                LONGLONG timestamp{};
                auto status=video.Tick(&timestamp);
                if(FAILED(status)) {
                    std::cerr<<"builtin failure 0x"<<std::hex<<static_cast<unsigned long>(status)<<std::dec
                        <<' '<<video.FailureReason()<<'\n';
                    throw std::runtime_error("builtin decoder failed");
                }
                if(status==S_OK && timestamp>=minimum)return timestamp;
                Sleep(2);
            }
            throw std::runtime_error("builtin frame deadline exceeded");
        }
        std::vector<BYTE> Pixels(DWORD& width,DWORD& height) {
            Check(SUCCEEDED(video.Size(&width,&height)),"builtin dimensions unavailable");
            D3D11_TEXTURE2D_DESC description{};
            description.Width=width;description.Height=height;
            description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
            description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            description.BindFlags=D3D11_BIND_RENDER_TARGET;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> target,staging;
            Check(SUCCEEDED(device->CreateTexture2D(&description,nullptr,&target)),"builtin target creation failed");
            Check(SUCCEEDED(video.Draw(target.Get(),{0,0,1,1})),"builtin GPU color conversion failed");
            description.BindFlags=0;description.Usage=D3D11_USAGE_STAGING;
            description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(SUCCEEDED(device->CreateTexture2D(&description,nullptr,&staging)),"builtin staging creation failed");
            context->CopyResource(staging.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"builtin readback failed");
            std::vector<BYTE> pixels(static_cast<size_t>(width)*height*4);
            for(DWORD row=0;row<height;++row) memcpy(pixels.data()+static_cast<size_t>(row)*width*4,
                static_cast<BYTE*>(mapped.pData)+static_cast<size_t>(row)*mapped.RowPitch,width*4);
            context->Unmap(staging.Get(),0);
            return pixels;
        }
        void Snapshot(std::filesystem::path const& path) {
            DWORD width{},height{};
            auto pixels=Pixels(width,height);
            BITMAPFILEHEADER file{};
            BITMAPINFOHEADER info{};
            file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
            file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
            info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-static_cast<LONG>(height);
            info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
            std::ofstream output(path,std::ios::binary);
            output.write(reinterpret_cast<char const*>(&file),sizeof(file));
            output.write(reinterpret_cast<char const*>(&info),sizeof(info));
            output.write(reinterpret_cast<char const*>(pixels.data()),pixels.size());
            Check(static_cast<bool>(output),"builtin snapshot write failed");
            std::cout<<"size="<<width<<'x'<<height<<" hardware="<<video.Hardware()<<'\n';
        }
    };

    // A test-only command lives in the test host, never in the shipped app.
    inline std::optional<int> handle_builtin_video_test_command(int argc,wchar_t** argv) {
        if(argc!=6 || std::wstring_view(argv[1])!=L"--probe-builtin")return {};
        try {
            bool hardware=std::wstring_view(argv[4])==L"hardware";
            BuiltinVideoFixture fixture(hardware);
            double seek=_wtof(argv[5]);
            BuiltinVideoFixture::Check(fixture.video.Start(argv[2],fixture.device.Get(),hardware,!hardware,60,seek),"builtin libraries unavailable");
            fixture.video.Play();
            auto timestamp=fixture.WaitFrame(static_cast<LONGLONG>(seek*10'000'000));
            std::cout<<"timestamp="<<timestamp<<'\n';
            fixture.video.Pause();fixture.Snapshot(argv[3]);
            return 0;
        } catch(std::exception const& error) {std::cerr<<error.what()<<'\n';return 3;}
    }

    template<typename Require>
    void builtin_video_clock_pixels_and_resume(std::filesystem::path const& root,Require require) {
        BuiltinVideoFixture fixture(false);
        auto source=root/L"decode-probe-h264.mp4";
        require(fixture.video.Start(source.wstring(),fixture.device.Get(),false,true,60),"builtin libraries did not load");
        fixture.video.Play();
        (void)fixture.WaitFrame(4'000'000);
        fixture.video.Pause();
        auto paused=fixture.video.CurrentTime();
        Sleep(100);
        require(std::abs(fixture.video.CurrentTime()-paused)<.0001,"builtin pause clock kept advancing");
        DWORD width{},height{};
        auto pixels=fixture.Pixels(width,height);
        require(width==64 && height==64 && !fixture.video.Hardware(),"CPU fixture changed dimensions or selected hardware");
        // This near-black limited-range fixture becomes approximately 3/255
        // under the display-referred BT.1886 -> sRGB conversion.
        require(pixels[0]>0 && pixels[0]<20 && std::abs(int(pixels[0])-pixels[1])<=2 &&
            std::abs(int(pixels[1])-pixels[2])<=2 && pixels[3]==255,"builtin YUV shader lost grayscale or alpha");
        fixture.video.Play();
        (void)fixture.WaitFrame(12'000'000); // Cross the one-second loop boundary.
        fixture.video.Pause();
        auto resume=fixture.video.CurrentTime();
        fixture.video.Shutdown(); // Same release/reopen sequence as idle compaction.
        require(fixture.video.Start(source.wstring(),fixture.device.Get(),false,true,60,resume),"builtin reopen failed");
        fixture.video.Play();
        auto resumed=fixture.WaitFrame(static_cast<LONGLONG>(resume*10'000'000));
        require(std::abs(resumed/10'000'000.0-resume)<.07,"builtin compacted resume restarted at the beginning");
        fixture.video.Shutdown();
        require(fixture.video.Start((root/L"decode-probe-invalid.mp4").wstring(),fixture.device.Get(),false,true,60),"invalid fixture could not start worker");
        fixture.video.Play();
        HRESULT status=S_FALSE; LONGLONG timestamp{};
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!FAILED(status) && std::chrono::steady_clock::now()<deadline) {status=fixture.video.Tick(&timestamp);Sleep(5);}
        require(FAILED(status),"malformed file never reported a bounded error");
    }
}
