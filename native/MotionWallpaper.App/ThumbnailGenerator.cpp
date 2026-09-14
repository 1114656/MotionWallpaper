#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
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
                ffmpeg.wstring(), L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y",
                L"-ss", L"0", L"-i", source.wstring(), L"-map", L"0:v:0", L"-frames:v", L"1",
                L"-vf", L"scale=480:270:force_original_aspect_ratio=decrease",
                L"-pix_fmt", L"rgb24", L"-f", L"image2", temporary.wstring()
            };
            auto command = motion::build_command_line(arguments);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, ffmpeg.parent_path().c_str(),
                &startup, &process)) return false;
            motion::unique_handle processHandle(process.hProcess);
            motion::unique_handle threadHandle(process.hThread);
            motion::unique_handle job(CreateJobObjectW(nullptr, nullptr));
            if (job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                    !AssignProcessToJobObject(job.get(), processHandle.get())) job.reset();
            }
            auto wait = WaitForSingleObject(processHandle.get(), 30'000);
            if (wait != WAIT_OBJECT_0) {
                TerminateProcess(processHandle.get(), ERROR_TIMEOUT);
                WaitForSingleObject(processHandle.get(), 2000);
                fs::remove(temporary, ignored);
                return false;
            }
            DWORD exitCode{};
            if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode || !suitable_cover(temporary)) {
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
        if (FAILED(MFStartup(MF_VERSION))) return false;
        bool succeeded = false;
        auto temporary = destination;
        temporary += L".tmp";
        try {
            winrt::com_ptr<IMFAttributes> attributes;
            winrt::check_hresult(MFCreateAttributes(attributes.put(), 2));
            winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
            winrt::check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
            winrt::com_ptr<IMFSourceReader> reader;
            winrt::check_hresult(MFCreateSourceReaderFromURL(source.c_str(), attributes.get(), reader.put()));
            winrt::check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
            winrt::check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE));

            winrt::com_ptr<IMFMediaType> native;
            winrt::check_hresult(reader->GetNativeMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, native.put()));
            UINT nativeWidth{}, nativeHeight{};
            winrt::check_hresult(MFGetAttributeSize(
                native.get(), MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight));
            auto [requestedWidth, requestedHeight] = thumbnail_size(nativeWidth, nativeHeight);

            winrt::com_ptr<IMFMediaType> requested;
            winrt::check_hresult(MFCreateMediaType(requested.put()));
            winrt::check_hresult(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            winrt::check_hresult(requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
            winrt::check_hresult(MFSetAttributeSize(
                requested.get(), MF_MT_FRAME_SIZE, requestedWidth, requestedHeight));
            winrt::check_hresult(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, requested.get()));

            winrt::com_ptr<IMFSample> sample;
            for (int attempt = 0; attempt < 120 && !sample; ++attempt) {
                DWORD stream{}, flags{};
                LONGLONG timestamp{};
                winrt::check_hresult(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &timestamp, sample.put()));
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            if (!sample) throw winrt::hresult_error(E_FAIL);

            winrt::com_ptr<IMFMediaType> actual;
            winrt::check_hresult(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actual.put()));
            UINT width{}, height{};
            winrt::check_hresult(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &width, &height));
            LONG sourceStride{};
            UINT32 strideValue{};
            if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue))) sourceStride = static_cast<LONG>(strideValue);
            else winrt::check_hresult(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &sourceStride));

            winrt::com_ptr<IMFMediaBuffer> buffer;
            winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
            BYTE* sourceBytes{};
            DWORD maximum{}, current{};
            winrt::check_hresult(buffer->Lock(&sourceBytes, &maximum, &current));
            bool bufferLocked = true;
            UINT targetStride{};
            std::vector<BYTE> pixels;
            try {
                if (width > UINT_MAX / 4) throw winrt::hresult_error(E_INVALIDARG);
                targetStride = width * 4;
                uint64_t pixelBytes = static_cast<uint64_t>(targetStride) * height;
                uint64_t sourceRowBytes = sourceStride < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(sourceStride)) : static_cast<uint64_t>(sourceStride);
                uint64_t requiredBytes = height ? sourceRowBytes * (height - 1) + targetStride : 0;
                if (!height || pixelBytes > UINT_MAX || sourceRowBytes < targetStride || requiredBytes > current) {
                    throw winrt::hresult_error(E_INVALIDARG);
                }
                pixels.resize(static_cast<size_t>(pixelBytes));
                auto row = sourceBytes;
                if (sourceStride < 0) row += static_cast<size_t>(height - 1) * static_cast<size_t>(-static_cast<int64_t>(sourceStride));
                for (UINT y = 0; y < height; ++y) {
                    memcpy(pixels.data() + static_cast<size_t>(y) * targetStride, row, targetStride);
                    row += sourceStride;
                }
                auto unlockResult = buffer->Unlock();
                bufferLocked = false;
                winrt::check_hresult(unlockResult);
            } catch (...) {
                if (bufferLocked) buffer->Unlock();
                throw;
            }

            auto imaging = imaging_factory();
            winrt::com_ptr<IWICBitmap> bitmap;
            winrt::check_hresult(imaging->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR,
                targetStride, static_cast<UINT>(pixels.size()), pixels.data(), bitmap.put()));
            write_thumbnail(bitmap.get(), destination);
            succeeded = true;
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
        MFShutdown();
        return succeeded || generate_video_cover_with_ffmpeg(source, destination);
    }
}
