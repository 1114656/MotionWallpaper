#pragma once
#include <windows.h>
#include <filesystem>
#include <mutex>
#pragma warning(push)
#pragma warning(disable: 4244) // Narrowing in the pinned FFmpeg C inline helpers.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
}
#pragma warning(pop)

namespace motion::renderer {
    class FfmpegApi {
    public:
#define MOTION_AV_FUNCTION(name) decltype(&::name) name{}
        MOTION_AV_FUNCTION(avformat_alloc_context);
        MOTION_AV_FUNCTION(avformat_open_input);
        MOTION_AV_FUNCTION(avformat_find_stream_info);
        MOTION_AV_FUNCTION(av_find_best_stream);
        MOTION_AV_FUNCTION(av_read_frame);
        MOTION_AV_FUNCTION(av_seek_frame);
        MOTION_AV_FUNCTION(avformat_close_input);
        MOTION_AV_FUNCTION(avcodec_find_decoder);
        MOTION_AV_FUNCTION(avcodec_alloc_context3);
        MOTION_AV_FUNCTION(avcodec_parameters_to_context);
        MOTION_AV_FUNCTION(avcodec_open2);
        MOTION_AV_FUNCTION(avcodec_send_packet);
        MOTION_AV_FUNCTION(avcodec_receive_frame);
        MOTION_AV_FUNCTION(avcodec_flush_buffers);
        MOTION_AV_FUNCTION(avcodec_free_context);
        MOTION_AV_FUNCTION(av_packet_alloc);
        MOTION_AV_FUNCTION(av_packet_free);
        MOTION_AV_FUNCTION(av_packet_unref);
        MOTION_AV_FUNCTION(av_packet_side_data_get);
        MOTION_AV_FUNCTION(av_frame_alloc);
        MOTION_AV_FUNCTION(av_frame_free);
        MOTION_AV_FUNCTION(av_frame_clone);
        MOTION_AV_FUNCTION(av_frame_unref);
        MOTION_AV_FUNCTION(av_frame_get_side_data);
        MOTION_AV_FUNCTION(av_buffer_ref);
        MOTION_AV_FUNCTION(av_buffer_unref);
        MOTION_AV_FUNCTION(av_hwdevice_ctx_alloc);
        MOTION_AV_FUNCTION(av_hwdevice_ctx_init);
        MOTION_AV_FUNCTION(av_dict_set);
        MOTION_AV_FUNCTION(av_dict_free);
        MOTION_AV_FUNCTION(av_display_rotation_get);
        MOTION_AV_FUNCTION(sws_getContext);
        MOTION_AV_FUNCTION(sws_scale);
        MOTION_AV_FUNCTION(sws_freeContext);
#undef MOTION_AV_FUNCTION
        static FfmpegApi& Get() { static FfmpegApi api; return api; }
        bool Load() {
            std::call_once(once_, [&] {
                wchar_t exe[32768]{};
                if (!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe))) return;
                auto directory = std::filesystem::path(exe).parent_path() / L"Tools" / L"ffmpeg";
                auto load = [&](wchar_t const* name) {
                    return LoadLibraryExW((directory / name).c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                };
                // Retain modules for the renderer lifetime; no global codec
                // registration and no change to the DLL search path.
                auto util = load(L"avutil-60.dll");
                auto codec = load(L"avcodec-62.dll");
                auto format = load(L"avformat-62.dll");
                auto scale = load(L"swscale-9.dll");
                if (!util || !codec || !format || !scale) return;
#define BIND(module, name) name = reinterpret_cast<decltype(name)>(GetProcAddress(module, #name)); if (!name) return
                BIND(format, avformat_alloc_context); BIND(format, avformat_open_input);
                BIND(format, avformat_find_stream_info); BIND(format, av_find_best_stream);
                BIND(format, av_read_frame); BIND(format, av_seek_frame); BIND(format, avformat_close_input);
                BIND(codec, avcodec_find_decoder); BIND(codec, avcodec_alloc_context3);
                BIND(codec, avcodec_parameters_to_context); BIND(codec, avcodec_open2);
                BIND(codec, avcodec_send_packet); BIND(codec, avcodec_receive_frame);
                BIND(codec, avcodec_flush_buffers); BIND(codec, avcodec_free_context);
                BIND(codec, av_packet_alloc); BIND(codec, av_packet_free); BIND(codec, av_packet_unref);
                BIND(codec, av_packet_side_data_get);
                BIND(util, av_frame_alloc); BIND(util, av_frame_free); BIND(util, av_frame_clone); BIND(util, av_frame_unref);
                BIND(util, av_frame_get_side_data);
                BIND(util, av_buffer_ref); BIND(util, av_buffer_unref);
                BIND(util, av_hwdevice_ctx_alloc); BIND(util, av_hwdevice_ctx_init);
                BIND(util, av_dict_set); BIND(util, av_dict_free); BIND(util, av_display_rotation_get);
                BIND(scale, sws_getContext); BIND(scale, sws_scale); BIND(scale, sws_freeContext);
#undef BIND
                loaded_ = true;
            });
            return loaded_;
        }
    private:
        std::once_flag once_;
        bool loaded_{};
    };
}
