#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "ThumbnailGenerator.h"
#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/MediaProbe.h"

namespace fs = std::filesystem;

namespace
{
    constexpr UINT coverMaxWidth = 480;
    constexpr UINT coverMaxHeight = 270;

    std::pair<UINT, UINT> thumbnail_size(UINT width, UINT height)
    {
        if (!width || !height || (width <= coverMaxWidth && height <= coverMaxHeight)) {
            return { width, height };
        }
        if (static_cast<uint64_t>(width) * coverMaxHeight >=
            static_cast<uint64_t>(height) * coverMaxWidth) {
            return { coverMaxWidth, (std::max)(1u, static_cast<UINT>(
                (static_cast<uint64_t>(height) * coverMaxWidth + width / 2) / width)) };
        }
        return { (std::max)(1u, static_cast<UINT>(
            (static_cast<uint64_t>(width) * coverMaxHeight + height / 2) / height)),
            coverMaxHeight };
    }

    winrt::com_ptr<IWICImagingFactory> imaging_factory()
    {
        winrt::com_ptr<IWICImagingFactory> result;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(result.put())));
        return result;
    }

    bool suitable_cover(fs::path const& path) noexcept
    {
        try {
            if (!fs::is_regular_file(path)) return false;
            auto imaging = imaging_factory();
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(imaging->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, decoder.put()));
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, frame.put()));
            UINT width{}, height{};
            winrt::check_hresult(frame->GetSize(&width, &height));
            WICPixelFormatGUID format{};
            winrt::check_hresult(frame->GetPixelFormat(&format));
            bool transparentLegacyCover = format == GUID_WICPixelFormat32bppBGRA || format == GUID_WICPixelFormat32bppPRGBA;
            return width > 0 && height > 0 && width <= coverMaxWidth && height <= coverMaxHeight && !transparentLegacyCover;
        } catch (...) {
            return false;
        }
    }

    void write_thumbnail(IWICBitmapSource* source, fs::path const& destination)
    {
        UINT width{}, height{};
        winrt::check_hresult(source->GetSize(&width, &height));
        if (!width || !height) throw winrt::hresult_error(E_INVALIDARG);

        auto [targetWidth, targetHeight] = thumbnail_size(width, height);

        auto imaging = imaging_factory();
        winrt::com_ptr<IWICBitmapSource> thumbnail;
        if (targetWidth == width && targetHeight == height) {
            source->AddRef();
            thumbnail.attach(source);
        } else {
            winrt::com_ptr<IWICBitmapScaler> scaler;
            winrt::check_hresult(imaging->CreateBitmapScaler(scaler.put()));
            winrt::check_hresult(scaler->Initialize(source, targetWidth, targetHeight, WICBitmapInterpolationModeFant));
            thumbnail = scaler.as<IWICBitmapSource>();
        }

        auto temporary = destination;
        temporary += L".tmp";
        std::error_code ignored;
        fs::remove(temporary, ignored);
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::com_ptr<IWICStream> stream;
        winrt::check_hresult(imaging->CreateStream(stream.put()));
        winrt::check_hresult(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        winrt::check_hresult(frame->SetSize(targetWidth, targetHeight));
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        winrt::check_hresult(frame->WriteSource(thumbnail.get(), nullptr));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
        // Some Windows WIC codecs retain the destination stream through the
        // frame object even after Commit(). Release every encoder-side owner
        // before the atomic rename, otherwise MoveFileEx can fail with
        // ERROR_SHARING_VIOLATION (notably for the lightweight BMP test input).
        frame = nullptr;
        stream = nullptr;
        encoder = nullptr;
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(temporary, ignored);
            winrt::throw_last_error();
        }
    }

    bool generate_video_cover_with_ffmpeg(fs::path const& source, fs::path const& destination) noexcept
    {
        try {
            auto ffmpeg = motion::executable_directory() / L"Tools" / L"ffmpeg" / L"ffmpeg.exe";
            if (!fs::is_regular_file(ffmpeg)) return false;
            auto temporary = destination.parent_path() / L"poster.ffmpeg.tmp.png";
            std::error_code ignored;
            fs::remove(temporary, ignored);
            auto arguments = std::vector<std::wstring>{
                L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y",
                L"-threads", L"2", L"-filter_threads", L"1",
                L"-ss", L"0", L"-i", source.wstring(), L"-map", L"0:v:0", L"-frames:v", L"1",
                L"-vf", L"scale=480:270:force_original_aspect_ratio=decrease",
                L"-pix_fmt", L"rgb24", L"-f", L"image2", temporary.wstring()
            };
            if (!motion::media_tool_detail::run_bounded(ffmpeg, arguments, 30'000) ||
                    !suitable_cover(temporary)) {
                fs::remove(temporary, ignored);
                return false;
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                fs::remove(temporary, ignored);
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }
}

namespace motion::app
{
    bool ThumbnailGenerator::EnsureImageCover(fs::path const& source, fs::path const& destination)
    {
        return suitable_cover(destination) || GenerateImageCover(source, destination);
    }

    bool ThumbnailGenerator::GenerateImageCover(fs::path const& source, fs::path const& destination)
    {
        try {
            auto imaging = imaging_factory();
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(imaging->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, decoder.put()));
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, frame.put()));
            write_thumbnail(frame.get(), destination);
            return true;
        } catch (...) {
            std::error_code ignored;
            auto temporary = destination;
            temporary += L".tmp";
            fs::remove(temporary, ignored);
            return false;
        }
    }

    bool ThumbnailGenerator::EnsureVideoCover(fs::path const& source, fs::path const& destination)
    {
        return suitable_cover(destination) || GenerateVideoCover(source, destination);
    }

    bool ThumbnailGenerator::GenerateVideoCover(fs::path const& source, fs::path const& destination)
    {
        // Third-party system decoders can block inside synchronous ReadSample.
        // Keep all video decoding in the bounded FFmpeg child, including the
        // first attempt; a missing tool or timeout leaves the cover unavailable.
        return generate_video_cover_with_ffmpeg(source, destination);
    }
}
