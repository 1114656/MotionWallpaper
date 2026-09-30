#pragma once
#include "../MotionWallpaper.Renderer/BuiltinVideo.h"
#include <d3d10_1.h>
#include <DirectXPackedVector.h>
#include "../MotionWallpaper.Common/MediaProbe.h"
#include "../MotionWallpaper.Common/TextEncoding.h"
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
        std::array<float,4> LinearPixel(bool hdrOutput=true, float sdrWhite=1.f) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=desc.Height=64; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> target,staging;
            Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)),"FP16 target unavailable");
            Check(SUCCEEDED(video.Draw(target.Get(),{0,0,1,1},sdrWhite,hdrOutput)),"scRGB conversion failed");
            desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"FP16 staging unavailable");
            context->CopyResource(staging.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"FP16 readback failed");
            auto pixel=reinterpret_cast<DirectX::PackedVector::HALF const*>(
                static_cast<BYTE const*>(mapped.pData)+32*mapped.RowPitch)+32*4;
            std::array<float,4> result{};
            for(unsigned c=0;c<4;++c)result[c]=DirectX::PackedVector::XMConvertHalfToFloat(pixel[c]);
            context->Unmap(staging.Get(),0);return result;
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
    template<typename Require>
    void builtin_scRGB_preserves_highlights_and_gamut(std::filesystem::path const& root,Require require) {
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        auto ffmpeg=std::filesystem::path(exe).parent_path()/L"Tools"/L"ffmpeg"/L"ffmpeg.exe";
        for(bool hdr : {false,true}) {
            auto path=root/(hdr?L"pq-white.mp4":L"p3-red.mp4");
            auto created=motion::media_tool_detail::run_bounded(ffmpeg,{
                L"-nostdin",L"-hide_banner",L"-loglevel",L"error",L"-y",L"-f",L"lavfi",L"-i",
                hdr?L"color=c=0xf0f0f0:s=64x64:r=30:d=1":L"color=c=red:s=64x64:r=30:d=1",
                L"-vf",L"scale=in_color_matrix=bt601:out_color_matrix=bt709:out_range=tv,format=yuv420p",L"-c:v",L"libopenh264",L"-b:v",L"200k",
                L"-color_primaries",hdr?L"bt2020":L"smpte432",L"-color_trc",hdr?L"smpte2084":L"bt470m",
                L"-colorspace",L"bt709",L"-color_range",L"tv",L"-bsf:v",
                hdr ? L"h264_metadata=colour_primaries=9:transfer_characteristics=16:matrix_coefficients=1:video_full_range_flag=0" :
                    L"h264_metadata=colour_primaries=12:transfer_characteristics=4:matrix_coefficients=1:video_full_range_flag=0",
                path.wstring()},10000);
            require(created.has_value(),"color shader fixture encoding failed");
            BuiltinVideoFixture fixture(false);
            require(fixture.video.Start(path.wstring(),fixture.device.Get(),false,true,30),"color fixture failed to load");
            fixture.video.Play();(void)fixture.WaitFrame();fixture.video.Pause();
            auto linear=fixture.LinearPixel();
            std::cout << "scRGB " << (hdr?"HDR":"P3") << " pixel=" << linear[0] << "," << linear[1] << "," << linear[2] << std::endl;
            if(hdr) {
                require(linear[0]>2 && linear[1]>2 && linear[2]>2,"HDR highlight was clipped into SDR");
                auto mapped=fixture.LinearPixel(false);
                require(mapped[0]<1.2f && mapped[0]<linear[0],"HDR on WCG SDR did not tone map");
            } else {
                require(linear[0]>1 && linear[1]<0,"P3 out-of-sRGB channels were clipped");
                auto brighter=fixture.LinearPixel(false,2.f);
                require(std::abs(brighter[0]-2*linear[0])<.005f,"Windows SDR white level was ignored");
            }
            DWORD width{},height{};auto pixels=fixture.Pixels(width,height);
            require(!pixels.empty(),"SDR fallback drawing failed");
        }

        // Preserved SDR transfers are also valid for 8-bit balanced copies.
        // A neutral midtone makes accidental BT.709 interpretation visible in
        // the actual GPU readback, without relying on a render-code string test.
        struct SdrTransfer { wchar_t const* name; wchar_t const* bitstream; double gamma; };
        for (auto const& transfer : {
            SdrTransfer{L"iec61966-2-1",L"13",0},
            SdrTransfer{L"bt470m",L"4",2.2},
            SdrTransfer{L"bt2020-10",L"14",2.4}}) {
            auto path=root/(std::wstring(L"sdr-midtone-")+transfer.name+L".mp4");
            auto bitstream=std::wstring(L"h264_metadata=colour_primaries=1:transfer_characteristics=")+
                transfer.bitstream+L":matrix_coefficients=1:video_full_range_flag=0";
            auto created=motion::media_tool_detail::run_bounded(ffmpeg,{
                L"-nostdin",L"-hide_banner",L"-loglevel",L"error",L"-y",L"-f",L"lavfi",L"-i",
                L"nullsrc=s=64x64:r=30:d=1,geq=lum=126:cb=128:cr=128,format=yuv420p",
                L"-c:v",L"libopenh264",L"-b:v",L"200k",L"-color_primaries",L"bt709",
                L"-color_trc",transfer.name,L"-colorspace",L"bt709",L"-color_range",L"tv",
                L"-bsf:v",bitstream,path.wstring()},10000);
            require(created.has_value(),"SDR transfer fixture encoding failed");
            auto info=motion::probe_video(ffmpeg,path,5000);
            require(info && info->bitDepth==8 && info->colorPrimaries=="bt709" &&
                info->colorTransfer==motion::utf8_from_wide(transfer.name),"SDR fixture lost its transfer metadata");
            // OpenH264 is lossy even for a constant color: use independently
            // decoded YUV samples as the reference, not the pre-encode values.
            auto decoded=motion::media_tool_detail::run_bounded(ffmpeg,{
                L"-nostdin",L"-hide_banner",L"-loglevel",L"error",L"-i",path.wstring(),
                L"-frames:v",L"1",L"-pix_fmt",L"yuv420p",L"-f",L"rawvideo",L"pipe:1"},10000);
            require(decoded && decoded->size()==64*64*3/2,"SDR reference decoding failed");
            double y=(static_cast<unsigned char>((*decoded)[32*64+32])-16.0)/219;
            double u=(static_cast<unsigned char>((*decoded)[64*64+16*32+16])-128.0)/224;
            double v=(static_cast<unsigned char>((*decoded)[64*64+32*32+16*32+16])-128.0)/224;
            constexpr double kr=.2126, kb=.0722, kg=1-kr-kb;
            std::array<double,3> expected{y+2*(1-kr)*v,
                y-2*kb*(1-kb)/kg*u-2*kr*(1-kr)/kg*v,y+2*(1-kb)*u};
            for(auto& channel:expected) {
                channel=(std::max)(channel,0.0);
                channel=transfer.gamma ? std::pow(channel,transfer.gamma) :
                    channel<=.04045 ? channel/12.92 : std::pow((channel+.055)/1.055,2.4);
            }
            BuiltinVideoFixture fixture(false);
            require(fixture.video.Start(path.wstring(),fixture.device.Get(),false,true,30),"8-bit SDR fixture failed to load");
            fixture.video.Play();(void)fixture.WaitFrame();fixture.video.Pause();
            auto linear=fixture.LinearPixel(false);
            std::cout << "SDR " << motion::utf8_from_wide(transfer.name) << " pixel=" << linear[0] << "," << linear[1]
                << "," << linear[2] << " expected=" << expected[0] << "," << expected[1] << "," << expected[2] << std::endl;
            for(unsigned channel=0;channel<3;++channel)
                require(std::abs(linear[channel]-expected[channel])<.003,"8-bit SDR midtone used the wrong transfer");
            DWORD width{},height{};auto pixels=fixture.Pixels(width,height);
            auto center=(static_cast<size_t>(height/2)*width+width/2)*4;
            require(pixels.size()>=center+4 && pixels[center+3]==255,"8-bit SDR fallback output lost alpha");
            for(unsigned channel=0;channel<3;++channel) {
                auto display=(1.055*std::pow(expected[channel],1/2.4)-.055)*255;
                require(std::abs(pixels[center+2-channel]-display)<2,
                    "8-bit SDR fallback output did not preserve the midtone");
            }
        }
    }

}
