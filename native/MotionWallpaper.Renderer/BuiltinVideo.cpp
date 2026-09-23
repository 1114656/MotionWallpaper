#include "BuiltinVideo.h"
#include "FfmpegApi.h"
#include "BuiltinVideoShader.h"
#include "../MotionWallpaper.Common/TextEncoding.h"
#include <d3dcompiler.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace motion::renderer {
namespace {
    double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    struct FrameDelete { void operator()(AVFrame* frame) const { if (frame) FfmpegApi::Get().av_frame_free(&frame); } };
    using Frame = std::unique_ptr<AVFrame, FrameDelete>;
    struct ShaderParameters { std::array<float,4> crop, range, yuv, gamut0, gamut1, gamut2, options, sampling; };
    bool color_parameters(AVFrame const& frame, bool tenBit, ShaderParameters& p) {
        auto& api=FfmpegApi::Get();
        if (api.av_frame_get_side_data(&frame,AV_FRAME_DATA_DOVI_METADATA)) return false;
        if(frame.color_trc==AVCOL_TRC_UNSPECIFIED &&
            (frame.color_primaries==AVCOL_PRI_BT2020 || frame.colorspace==AVCOL_SPC_BT2020_NCL)) return false;
        int transfer = frame.color_trc == AVCOL_TRC_UNSPECIFIED ? AVCOL_TRC_BT709 : frame.color_trc;
        if (transfer != AVCOL_TRC_BT709 && transfer != AVCOL_TRC_SMPTE170M && transfer != AVCOL_TRC_BT2020_10 &&
            transfer != AVCOL_TRC_BT2020_12 && transfer != AVCOL_TRC_GAMMA22 && transfer != AVCOL_TRC_GAMMA28 &&
            transfer != AVCOL_TRC_LINEAR && transfer != AVCOL_TRC_IEC61966_2_1 && transfer != AVCOL_TRC_SMPTE2084 &&
            transfer != AVCOL_TRC_ARIB_STD_B67) return false;
        float kr=.2126f, kb=.0722f;
        switch (frame.colorspace) {
        case AVCOL_SPC_BT2020_NCL: kr=.2627f; kb=.0593f; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: kr=.299f; kb=.114f; break;
        case AVCOL_SPC_BT709: case AVCOL_SPC_UNSPECIFIED: break;
        default: return false;
        }
        float kg=1-kr-kb;
        p.yuv={2*(1-kr),-2*kb*(1-kb)/kg,-2*kr*(1-kr)/kg,2*(1-kb)};
        bool full = frame.color_range == AVCOL_RANGE_JPEG;
        if (tenBit) {
            constexpr float unit=64.f/65535.f;
            p.range={full?0.f:64*unit,1/((full?1023:876)*unit),512*unit,1/((full?1023:896)*unit)};
        } else p.range={full?0.f:16.f/255.f,255.f/(full?255:219),128.f/255.f,255.f/(full?255:224)};
        p.gamut0={1,0,0,10}; p.gamut1={0,1,0,0}; p.gamut2={0,0,1,0};
        switch (frame.color_primaries) {
        case AVCOL_PRI_BT2020:
            p.gamut0={1.660491f,-.587641f,-.072850f,10};
            p.gamut1={-.124550f,1.132900f,-.008349f,0}; p.gamut2={-.018151f,-.100579f,1.118730f,0}; break;
        case AVCOL_PRI_SMPTE432:
            p.gamut0={1.224940f,-.224940f,0,10};
            p.gamut1={-.042057f,1.042057f,0,0}; p.gamut2={-.019638f,-.078636f,1.098274f,0}; break;
        case AVCOL_PRI_SMPTE170M:
            p.gamut0={.939542f,.050181f,.010277f,10};
            p.gamut1={.017772f,.965793f,.016435f,0}; p.gamut2={-.001622f,-.004370f,1.005992f,0}; break;
        case AVCOL_PRI_BT470BG:
            p.gamut0={1.044043f,-.044043f,0,10};
            p.gamut1={0,1,0,0}; p.gamut2={0,.011793f,.988207f,0}; break;
        case AVCOL_PRI_BT709: case AVCOL_PRI_UNSPECIFIED: break;
        default: return false;
        }
        p.options[0]=static_cast<float>(transfer);
        p.sampling[2]=.2126f*p.gamut0[0]+.7152f*p.gamut1[0]+.0722f*p.gamut2[0];
        p.sampling[3]=.2126f*p.gamut0[2]+.7152f*p.gamut1[2]+.0722f*p.gamut2[2];
        // PQ metadata describes absolute luminance. Use MaxCLL first, then
        // mastering peak; absent metadata uses a documented 1000-nit default.
        if (transfer==AVCOL_TRC_SMPTE2084) {
            double nits=1000;
            if (auto* side=api.av_frame_get_side_data(&frame,AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
                side && side->size>=sizeof(AVMasteringDisplayMetadata)) {
                auto* metadata=reinterpret_cast<AVMasteringDisplayMetadata const*>(side->data);
                if(metadata->has_luminance) nits=av_q2d(metadata->max_luminance);
            }
            if(auto* side=api.av_frame_get_side_data(&frame,AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
                side && side->size>=sizeof(AVContentLightMetadata)) {
                auto* metadata=reinterpret_cast<AVContentLightMetadata const*>(side->data);
                if(metadata->MaxCLL) nits=metadata->MaxCLL;
            }
            p.gamut0[3]=static_cast<float>(std::isfinite(nits)?std::clamp(nits/100,1.0,100.0):10);
        }
        return true;
    }
}

struct BuiltinVideo::Impl {
    FfmpegApi& api{FfmpegApi::Get()};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring path;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::atomic_bool stopped{}, hardware{};
    std::atomic<double> ioDeadline{};
    bool allowHardware{}, allowSoftware{}, playing{}, clockStarted{};
    double position{}, clockBase{}, duration{}, seekStart{}, rate{60};
    HRESULT error{S_OK};
    std::string failureReason{"builtin-decode-failed"};
    unsigned width{}, height{}, rotation{};
    double pixelAspect{1};
    struct TimedFrame { Frame frame; double time{}; };
    std::deque<TimedFrame> queue;
    Frame current;
    double currentTimestamp{};
    uint64_t serial{}, uploadedSerial{};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> yView, uvView;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    std::vector<uint8_t> packed;
    SwsContext* scaler{};
    unsigned packedWidth{}, packedHeight{};
    AVPixelFormat packedFormat{AV_PIX_FMT_NONE};
    ShaderParameters params{};

    double Clock() const { return position + (playing && clockStarted ? now_seconds()-clockBase : 0); }
    void Fail(HRESULT hr, int ffError=0, char const* reason="builtin-decode-failed") {
        std::lock_guard lock(mutex); error=hr; failureReason=reason;
        std::cerr << "builtin decoder failure 0x" << std::hex << static_cast<unsigned long>(hr)
            << std::dec << " ffmpeg=" << ffError << '\n' << std::flush;
        changed.notify_all();
    }
    static int Interrupt(void* data) {
        auto& self=*static_cast<Impl*>(data);
        return self.stopped || (self.ioDeadline.load()>0 && now_seconds()>self.ioDeadline.load());
    }
    static AVPixelFormat HardwareFormat(AVCodecContext*, AVPixelFormat const* formats) {
        for (; *formats != AV_PIX_FMT_NONE; ++formats) if (*formats == AV_PIX_FMT_D3D11) return *formats;
        return AV_PIX_FMT_NONE;
    }
    void Run() {
        AVFormatContext* input=api.avformat_alloc_context();
        AVCodecContext* decoder{};
        AVBufferRef* hw{};
        AVPacket* packet=api.av_packet_alloc();
        Frame decoded(api.av_frame_alloc());
        struct Cleanup {
            Impl& self; AVFormatContext*& input; AVCodecContext*& decoder; AVBufferRef*& hw; AVPacket*& packet;
            ~Cleanup(){ self.api.avcodec_free_context(&decoder); self.api.av_buffer_unref(&hw);
                self.api.avformat_close_input(&input); self.api.av_packet_free(&packet); }
        } cleanup{*this,input,decoder,hw,packet};
        if (!input || !packet || !decoded) { Fail(E_OUTOFMEMORY); return; }
        input->interrupt_callback={Interrupt,this};
        AVDictionary* options{};
        api.av_dict_set(&options,"protocol_whitelist","file",0);
        api.av_dict_set(&options,"probesize","5242880",0);
        api.av_dict_set(&options,"analyzeduration","3000000",0);
        ioDeadline=now_seconds()+15;
        int status=api.avformat_open_input(&input,motion::utf8_from_wide(path).c_str(),nullptr,&options);
        api.av_dict_free(&options);
        if (status>=0) status=api.avformat_find_stream_info(input,nullptr);
        ioDeadline=0;
        if (status<0) { if(!stopped) Fail(MF_E_INVALID_FILE_FORMAT,status); return; }
        int streamIndex=api.av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        if (streamIndex<0) { Fail(MF_E_INVALIDMEDIATYPE,streamIndex); return; }
        auto* stream=input->streams[streamIndex];
        auto* description=stream->codecpar;
        // Initial fallback scope is HEVC/H.264. Never let a malformed header
        // allocate unbounded decoder surfaces or silently change resolution.
        if ((description->codec_id!=AV_CODEC_ID_HEVC && description->codec_id!=AV_CODEC_ID_H264) ||
            description->width<=0 || description->height<=0 || description->width>8192 || description->height>8192 ||
            int64_t(description->width)*description->height>8192LL*4320) { Fail(MF_E_INVALIDMEDIATYPE); return; }
        auto fps=av_q2d(stream->avg_frame_rate);
        if (!std::isfinite(fps) || fps<=0) fps=30;
        if (fps>241) { Fail(MF_E_INVALIDMEDIATYPE); return; }
        double timebase=av_q2d(stream->time_base);
        double origin=stream->start_time==AV_NOPTS_VALUE ? 0 : stream->start_time*timebase;
        double clipDuration=stream->duration!=AV_NOPTS_VALUE ? stream->duration*timebase :
            input->duration!=AV_NOPTS_VALUE ? static_cast<double>(input->duration)/AV_TIME_BASE : 0;
        if (!std::isfinite(timebase) || timebase<=0 || !std::isfinite(clipDuration) || clipDuration<0) {
            Fail(MF_E_INVALIDMEDIATYPE); return;
        }
        unsigned sourceRotation{};
        auto* matrix=api.av_packet_side_data_get(description->coded_side_data,description->nb_coded_side_data,AV_PKT_DATA_DISPLAYMATRIX);
        if (matrix) {
            if (matrix->size<9*sizeof(int32_t)) { Fail(MF_E_INVALIDMEDIATYPE); return; }
            auto* transform=reinterpret_cast<int32_t const*>(matrix->data);
            double a=transform[0], b=transform[1], c=transform[3], d=transform[4];
            // Rotation only: reject mirrored, sheared, perspective and
            // anisotropic display matrices instead of showing a wrong image.
            double scale=a*a+b*b;
            if (a*d-b*c<=0 || scale<=0 || std::abs(a*c+b*d)>scale*.001 ||
                std::abs(scale-c*c-d*d)>scale*.001 || transform[2] || transform[5]) {
                Fail(MF_E_INVALIDMEDIATYPE); return;
            }
            double degrees=-api.av_display_rotation_get(transform);
            if (!std::isfinite(degrees) || std::abs(degrees-std::round(degrees/90)*90)>.01) { Fail(MF_E_INVALIDMEDIATYPE); return; }
            sourceRotation=static_cast<unsigned>((static_cast<int>(std::lround(degrees))+720)%360);
        }
        {
            std::lock_guard lock(mutex);
            width=static_cast<unsigned>(description->width); height=static_cast<unsigned>(description->height);
            rotation=sourceRotation; duration=clipDuration;
            if (description->sample_aspect_ratio.num>0 && description->sample_aspect_ratio.den>0)
                pixelAspect=av_q2d(description->sample_aspect_ratio);
            if (!std::isfinite(pixelAspect) || pixelAspect<.1 || pixelAspect>10) { error=MF_E_INVALIDMEDIATYPE; return; }
        }
        bool softwareBudget=int64_t(description->width)*description->height<=1920LL*1080 && fps<=60.01;
        if (!allowHardware && (!allowSoftware || !softwareBudget)) {
            Fail(MF_E_INVALIDMEDIATYPE,0,"builtin-software-budget"); return;
        }
        auto openDecoder=[&](bool useHardware) {
            api.avcodec_free_context(&decoder); api.av_buffer_unref(&hw);
            auto* codec=api.avcodec_find_decoder(description->codec_id);
            decoder=codec ? api.avcodec_alloc_context3(codec) : nullptr;
            if (!decoder || api.avcodec_parameters_to_context(decoder,description)<0) return false;
            decoder->pkt_timebase=stream->time_base;
            decoder->thread_count=useHardware?1:2;
            decoder->max_pixels=useHardware?8192LL*4320:1920LL*1080;
            decoder->extra_hw_frames=4;
            // High-frame-rate originals often contain disposable B pictures.
            // Keep every reference picture, but avoid decoding disposable
            // pictures when the display cannot show even half the source rate.
            decoder->skip_frame=fps>rate*2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (useHardware) {
                hw=api.av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                if (!hw) return false;
                auto* hwDevice=reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hw->data)->hwctx);
                hwDevice->device=device.Get(); device->AddRef();
                if (api.av_hwdevice_ctx_init(hw)<0) return false;
                decoder->hw_device_ctx=api.av_buffer_ref(hw);
                decoder->get_format=HardwareFormat;
            }
            return api.avcodec_open2(decoder,codec,nullptr)>=0;
        };
        bool useHardware=allowHardware;
        if (!openDecoder(useHardware)) {
            useHardware=false;
            if (!allowSoftware || !softwareBudget || !openDecoder(false)) { Fail(MF_E_TOPO_CODEC_NOT_FOUND); return; }
        }
        auto seek=[&](double seconds) {
            ioDeadline=now_seconds()+10;
            int value=api.av_seek_frame(input,streamIndex,static_cast<int64_t>((seconds+origin)/timebase),AVSEEK_FLAG_BACKWARD);
            ioDeadline=0;
            if(value>=0) api.avcodec_flush_buffers(decoder);
            return value>=0;
        };
        if (seekStart>0 && !seek(seekStart)) { Fail(MF_E_INVALIDREQUEST); return; }
        double loopOffset{}, lastSelected=-1e10, lastTime=-1, guessedTime{};
        bool drained{}, sentEnd{}, acceptedFrame{};
        while (!stopped) {
            {
                std::unique_lock lock(mutex);
                changed.wait(lock,[&]{return stopped || (playing && queue.size()<3);});
                if(stopped) return;
            }
            status=api.avcodec_receive_frame(decoder,decoded.get());
            if (status>=0) {
                if(decoded->width!=description->width || decoded->height!=description->height ||
                    decoded->crop_top || decoded->crop_bottom || decoded->crop_left || decoded->crop_right) {
                    Fail(MF_E_INVALIDMEDIATYPE); return;
                }
                // MOV colr metadata is also used by the performance-copy and
                // still-image paths. Some Apple files have generic BT.709 VUI
                // inside an explicitly tagged P3/gamma-2.2 container.
                if(description->color_primaries!=AVCOL_PRI_UNSPECIFIED) decoded->color_primaries=description->color_primaries;
                if(description->color_trc!=AVCOL_TRC_UNSPECIFIED) decoded->color_trc=description->color_trc;
                if(description->color_space!=AVCOL_SPC_UNSPECIFIED) decoded->colorspace=description->color_space;
                if(description->color_range!=AVCOL_RANGE_UNSPECIFIED) decoded->color_range=description->color_range;
                double local=decoded->best_effort_timestamp!=AV_NOPTS_VALUE ? decoded->best_effort_timestamp*timebase-origin : guessedTime;
                guessedTime=local+1/fps;
                if (!std::isfinite(local) || local<-.1 || (clipDuration>0 && local>clipDuration+1)) { Fail(MF_E_INVALID_TIMESTAMP); return; }
                local=(std::max)(0.0,local); lastTime=local;
                double timeline=loopOffset+local;
                if(timeline+1e-5<seekStart || timeline-lastSelected+1e-6<1/rate) {api.av_frame_unref(decoded.get());continue;}
                {
                    std::unique_lock lock(mutex);
                    if(clockStarted && timeline<Clock()-.1) {
                        // Never spin forever decoding an ever-growing backlog
                        // on hardware unable to sustain the reference stream.
                        if (Clock()-timeline>3) {
                            error=MF_E_UNSUPPORTED_RATE; failureReason="builtin-throughput-insufficient"; return;
                        }
                        api.av_frame_unref(decoded.get());continue;
                    }
                    Frame copy(api.av_frame_clone(decoded.get()));
                    if(!copy){error=E_OUTOFMEMORY;return;}
                    queue.push_back({std::move(copy),timeline});
                }
                hardware=useHardware; acceptedFrame=true; lastSelected=timeline;
                api.av_frame_unref(decoded.get());
                changed.notify_all();
                continue;
            }
            if(status==AVERROR_EOF) {
                if(!acceptedFrame || lastTime<0) {Fail(MF_E_INVALID_FILE_FORMAT);return;}
                loopOffset+=(std::max)(clipDuration,lastTime+1/fps);
                if(!seek(0)){Fail(MF_E_INVALIDREQUEST);return;}
                sentEnd=drained=false; guessedTime=0; lastTime=-1;
                continue;
            }
            if(status!=AVERROR(EAGAIN)) {
                if(useHardware && !acceptedFrame && allowSoftware && softwareBudget && openDecoder(false) && seek(seekStart)) {
                    useHardware=false; sentEnd=drained=false; api.av_packet_unref(packet); continue;
                }
                Fail(MF_E_TOPO_CODEC_NOT_FOUND,status);return;
            }
            if(drained) {
                if(sentEnd) {Fail(MF_E_INVALID_FILE_FORMAT);return;}
                status=api.avcodec_send_packet(decoder,nullptr); sentEnd=true;
            } else {
                api.av_packet_unref(packet);
                ioDeadline=now_seconds()+10;
                status=api.av_read_frame(input,packet);
                ioDeadline=0;
                if(status==AVERROR_EOF){drained=true;continue;}
                if(status<0){if(!stopped)Fail(MF_E_INVALID_FILE_FORMAT,status);return;}
                if(packet->stream_index!=streamIndex) continue;
                status=api.avcodec_send_packet(decoder,packet);
            }
            // receive/send cannot both return EAGAIN. Fail safely rather than
            // silently discard a packet if a broken decoder violates this.
            if(status<0) {
                if(useHardware && !acceptedFrame && allowSoftware && softwareBudget && openDecoder(false) && seek(seekStart)) {
                    useHardware=false; sentEnd=drained=false; continue;
                }
                Fail(MF_E_TOPO_CODEC_NOT_FOUND,status);return;
            }
        }
    }
    bool CreateShaders() {
        if(vertex) return true;
        ComPtr<ID3DBlob> vs,ps,errors;
        if(FAILED(D3DCompile(builtin_video_shader,sizeof(builtin_video_shader)-1,nullptr,nullptr,nullptr,"vs","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors)) ||
            FAILED(D3DCompile(builtin_video_shader,sizeof(builtin_video_shader)-1,nullptr,nullptr,nullptr,"ps","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errors))) {
            if(errors) std::cerr.write(static_cast<char const*>(errors->GetBufferPointer()),errors->GetBufferSize());
            return false;
        }
        if(FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel))) return false;
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD=D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(params); buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateSamplerState(&sample,&sampler)) && SUCCEEDED(device->CreateBuffer(&buffer,nullptr,&constants));
    }
    HRESULT Upload() {
        if(uploadedSerial==serial) return S_OK;
        bool gpu=current->format==AV_PIX_FMT_D3D11;
        bool tenBit=current->format==AV_PIX_FMT_YUV420P10LE || current->format==AV_PIX_FMT_P010LE;
        D3D11_TEXTURE2D_DESC description{};
        ID3D11Texture2D* sourceTexture{};
        if(gpu) {
            sourceTexture=reinterpret_cast<ID3D11Texture2D*>(current->data[0]);
            if(!sourceTexture) return E_POINTER;
            sourceTexture->GetDesc(&description);
            if(description.Format!=DXGI_FORMAT_NV12 && description.Format!=DXGI_FORMAT_P010) return MF_E_INVALIDMEDIATYPE;
            tenBit=description.Format==DXGI_FORMAT_P010;
        } else {
            if(current->format!=AV_PIX_FMT_YUV420P && current->format!=AV_PIX_FMT_YUVJ420P && current->format!=AV_PIX_FMT_NV12 && !tenBit)
                return MF_E_INVALIDMEDIATYPE;
            if ((current->width&1) || (current->height&1)) return MF_E_INVALIDMEDIATYPE;
            description.Width=static_cast<UINT>(current->width); description.Height=static_cast<UINT>(current->height);
            description.Format=tenBit?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;
        }
        if(!color_parameters(*current,tenBit,params)) return MF_E_INVALIDMEDIATYPE;
        description.ArraySize=description.MipLevels=1; description.SampleDesc={1,0};
        description.Usage=D3D11_USAGE_DEFAULT; description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags=description.MiscFlags=0;
        D3D11_TEXTURE2D_DESC previous{}; if(texture)texture->GetDesc(&previous);
        if(!texture || previous.Width!=description.Width || previous.Height!=description.Height || previous.Format!=description.Format) {
            texture.Reset(); yView.Reset(); uvView.Reset();
            HRESULT hr=device->CreateTexture2D(&description,nullptr,&texture); if(FAILED(hr))return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
            view.Format=tenBit?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R8_UNORM;
            hr=device->CreateShaderResourceView(texture.Get(),&view,&yView); if(FAILED(hr))return hr;
            view.Format=tenBit?DXGI_FORMAT_R16G16_UNORM:DXGI_FORMAT_R8G8_UNORM;
            hr=device->CreateShaderResourceView(texture.Get(),&view,&uvView); if(FAILED(hr))return hr;
        }
        if(gpu) {
            context->CopySubresourceRegion(texture.Get(),0,0,0,0,sourceTexture,
                static_cast<UINT>(reinterpret_cast<uintptr_t>(current->data[1])),nullptr);
        } else {
            unsigned stride=description.Width*(tenBit?2:1);
            packed.resize(static_cast<size_t>(stride)*description.Height*3/2);
            if(!scaler || packedWidth!=description.Width || packedHeight!=description.Height || packedFormat!=current->format) {
                if(scaler)api.sws_freeContext(scaler);
                scaler=api.sws_getContext(current->width,current->height,static_cast<AVPixelFormat>(current->format),
                    current->width,current->height,tenBit?AV_PIX_FMT_P010LE:AV_PIX_FMT_NV12,SWS_POINT,nullptr,nullptr,nullptr);
                packedWidth=description.Width; packedHeight=description.Height; packedFormat=static_cast<AVPixelFormat>(current->format);
            }
            if(!scaler)return E_OUTOFMEMORY;
            uint8_t* planes[]{packed.data(),packed.data()+static_cast<size_t>(stride)*description.Height,nullptr,nullptr};
            int strides[]{static_cast<int>(stride),static_cast<int>(stride),0,0};
            if(api.sws_scale(scaler,current->data,current->linesize,0,current->height,planes,strides)!=current->height)return E_FAIL;
            context->UpdateSubresource(texture.Get(),0,nullptr,packed.data(),stride,0);
        }
        params.options[1]=static_cast<float>(rotation);
        params.options[2]=static_cast<float>(current->width)/description.Width;
        params.options[3]=static_cast<float>(current->height)/description.Height;
        auto location=current->chroma_location;
        if(location==AVCHROMA_LOC_UNSPECIFIED) location=AVCHROMA_LOC_LEFT;
        params.sampling[0]=(location==AVCHROMA_LOC_LEFT || location==AVCHROMA_LOC_TOPLEFT ||
            location==AVCHROMA_LOC_BOTTOMLEFT) ? .5f/description.Width : 0;
        params.sampling[1]=(location==AVCHROMA_LOC_TOP || location==AVCHROMA_LOC_TOPLEFT) ? .5f/description.Height :
            (location==AVCHROMA_LOC_BOTTOM || location==AVCHROMA_LOC_BOTTOMLEFT) ? -.5f/description.Height : 0;
        uploadedSerial=serial;
        return S_OK;
    }
    ~Impl(){stopped=true;changed.notify_all();if(worker.joinable())worker.join();if(scaler)api.sws_freeContext(scaler);}
};

BuiltinVideo::BuiltinVideo()=default;
BuiltinVideo::~BuiltinVideo()=default;
bool BuiltinVideo::Start(std::wstring const& path,ID3D11Device* device,bool hardware,bool allowSoftware,unsigned displayRate,double resumeSeconds) {
    Shutdown();
    if(!device || !FfmpegApi::Get().Load())return false;
    impl_=std::make_unique<Impl>(); auto& self=*impl_;
    self.device=device; device->GetImmediateContext(&self.context); self.path=path;
    self.allowHardware=hardware; self.allowSoftware=allowSoftware;
    self.rate=std::clamp(displayRate,1u,240u); self.position=self.seekStart=(std::max)(0.0,resumeSeconds);
    self.worker=std::thread([&self]{try{self.Run();}catch(...){self.Fail(E_FAIL);}});
    return true;
}
HRESULT BuiltinVideo::Tick(LONGLONG* timestamp) {
    if(!impl_ || !timestamp)return E_POINTER;
    auto& self=*impl_; std::lock_guard lock(self.mutex);
    if(FAILED(self.error))return self.error;
    bool fresh{};
    if(!self.clockStarted && !self.queue.empty()) {self.clockStarted=true;self.clockBase=now_seconds();}
    double clock=self.Clock();
    while(!self.queue.empty() && self.queue.front().time<=clock+.002) {
        self.current=std::move(self.queue.front().frame); self.currentTimestamp=self.queue.front().time;
        self.queue.pop_front(); ++self.serial; fresh=true;
    }
    self.changed.notify_all();
    *timestamp=static_cast<LONGLONG>(self.currentTimestamp*10'000'000);
    return fresh?S_OK:S_FALSE;
}
HRESULT BuiltinVideo::Size(DWORD* width,DWORD* height) const {
    if(!impl_ || !width || !height)return E_POINTER;
    std::lock_guard lock(impl_->mutex);
    *width=static_cast<DWORD>(std::lround(impl_->width*impl_->pixelAspect)); *height=impl_->height;
    if(impl_->rotation==90 || impl_->rotation==270)std::swap(*width,*height);
    return *width && *height?S_OK:E_PENDING;
}
HRESULT BuiltinVideo::Draw(ID3D11Texture2D* destination,MFVideoNormalizedRect const& crop) {
    if(!impl_ || !destination || !impl_->current)return E_POINTER;
    auto& self=*impl_;
    if(!self.CreateShaders())return E_FAIL;
    HRESULT status=self.Upload(); if(FAILED(status))return status;
    D3D11_TEXTURE2D_DESC description{}; destination->GetDesc(&description);
    ComPtr<ID3D11RenderTargetView> target;
    status=self.device->CreateRenderTargetView(destination,nullptr,&target);if(FAILED(status))return status;
    self.params.crop={crop.left,crop.top,crop.right,crop.bottom};
    auto* context=self.context.Get();
    context->UpdateSubresource(self.constants.Get(),0,nullptr,&self.params,0,0);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(description.Width),static_cast<float>(description.Height),0,1};
    context->RSSetViewports(1,&viewport);
    context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);
    context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(self.vertex.Get(),nullptr,0);context->PSSetShader(self.pixel.Get(),nullptr,0);
    context->PSSetConstantBuffers(0,1,self.constants.GetAddressOf());context->PSSetSamplers(0,1,self.sampler.GetAddressOf());
    ID3D11ShaderResourceView* views[]{self.yView.Get(),self.uvView.Get()};context->PSSetShaderResources(0,2,views);
    context->Draw(3,0);
    ID3D11ShaderResourceView* clear[]{nullptr,nullptr};context->PSSetShaderResources(0,2,clear);
    context->OMSetRenderTargets(0,nullptr,nullptr);
    return self.device->GetDeviceRemovedReason();
}
void BuiltinVideo::Play(){if(impl_){std::lock_guard lock(impl_->mutex);if(!impl_->playing){impl_->clockBase=now_seconds();impl_->playing=true;}impl_->changed.notify_all();}}
void BuiltinVideo::Pause(){if(impl_){std::lock_guard lock(impl_->mutex);impl_->position=impl_->Clock();impl_->playing=false;impl_->changed.notify_all();}}
double BuiltinVideo::CurrentTime() const {if(!impl_)return 0;std::lock_guard lock(impl_->mutex);double value=impl_->Clock();return impl_->duration>0?std::fmod(value,impl_->duration):value;}
bool BuiltinVideo::Hardware() const {return impl_ && impl_->hardware;}
std::string BuiltinVideo::FailureReason() const {
    if(!impl_)return "builtin-decoder-unavailable";
    std::lock_guard lock(impl_->mutex);
    return FAILED(impl_->error)?impl_->failureReason:"builtin-presentation-failed";
}
void BuiltinVideo::Shutdown(){impl_.reset();}
}
