#pragma once

#include <d3d11.h>
#include <mfmediaengine.h>
#include <wincodec.h>
#include <wrl.h>

#include <cstdint>
#include <limits>

namespace motion::renderer
{
    // CPU Media Engine frames use a WIC bitmap and an independent presentation
    // device. Giving the decoder a DXGI manager would re-enable GPU decoding.
    class SoftwareVideoTransfer
    {
    public:
        HRESULT Transfer(IMFMediaEngine* engine, ID3D11DeviceContext* context,
            ID3D11Texture2D* destination, MFVideoNormalizedRect const& sourceRect,
            RECT const& destinationRect, MFARGB const& border)
        {
            if (!engine || !context || !destination) return E_POINTER;
            D3D11_TEXTURE2D_DESC description{};
            destination->GetDesc(&description);
            if (!ValidDestination(description) || destinationRect.left < 0 ||
                destinationRect.top < 0 || destinationRect.right <= destinationRect.left ||
                destinationRect.bottom <= destinationRect.top ||
                static_cast<UINT>(destinationRect.right) > description.Width ||
                static_cast<UINT>(destinationRect.bottom) > description.Height) return E_INVALIDARG;
            auto result = EnsureBitmap(description.Width, description.Height);
            if (FAILED(result)) return result;
            result = engine->TransferVideoFrame(bitmap_.Get(), &sourceRect, &destinationRect, &border);
            if (FAILED(result)) return result;
            return UploadBitmap(context, destination, bitmap_.Get());
        }

        static HRESULT UploadBitmap(ID3D11DeviceContext* context,
            ID3D11Texture2D* destination, IWICBitmap* bitmap)
        {
            if (!context || !destination || !bitmap) return E_POINTER;
            D3D11_TEXTURE2D_DESC description{};
            destination->GetDesc(&description);
            if (!ValidDestination(description)) return E_INVALIDARG;
            UINT width{}, height{};
            WICPixelFormatGUID format{};
            auto result = bitmap->GetSize(&width, &height);
            if (FAILED(result)) return result;
            result = bitmap->GetPixelFormat(&format);
            if (FAILED(result)) return result;
            if (width != description.Width || height != description.Height ||
                format != GUID_WICPixelFormat32bppBGRA) return E_INVALIDARG;
            Microsoft::WRL::ComPtr<ID3D11Device> contextDevice, textureDevice;
            context->GetDevice(&contextDevice);
            destination->GetDevice(&textureDevice);
            if (!contextDevice || contextDevice.Get() != textureDevice.Get()) return E_INVALIDARG;
            WICRect rectangle{ 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
            Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
            result = bitmap->Lock(&rectangle, WICBitmapLockRead, &lock);
            if (FAILED(result)) return result;
            UINT stride{}, size{};
            BYTE* pixels{};
            result = lock->GetStride(&stride);
            if (FAILED(result)) return result;
            result = lock->GetDataPointer(&size, &pixels);
            if (FAILED(result)) return result;
            uint64_t rowBytes = static_cast<uint64_t>(width) * 4;
            uint64_t required = static_cast<uint64_t>(height - 1) * stride + rowBytes;
            if (!pixels || stride < rowBytes || size < required) return E_INVALIDARG;
            context->UpdateSubresource(destination, 0, nullptr, pixels, stride, 0);
            return contextDevice->GetDeviceRemovedReason();
        }

        void Clear() noexcept
        {
            bitmap_.Reset();
            factory_.Reset();
            width_ = height_ = 0;
        }

    private:
        static bool ValidDestination(D3D11_TEXTURE2D_DESC const& value) noexcept
        {
            return value.Width && value.Height &&
                value.Width <= static_cast<UINT>((std::numeric_limits<INT>::max)()) &&
                value.Height <= static_cast<UINT>((std::numeric_limits<INT>::max)()) &&
                value.Format == DXGI_FORMAT_B8G8R8A8_UNORM && value.SampleDesc.Count == 1 &&
                value.ArraySize == 1 && value.MipLevels == 1 && value.Usage == D3D11_USAGE_DEFAULT;
        }

        HRESULT EnsureBitmap(UINT width, UINT height)
        {
            if (bitmap_ && width_ == width && height_ == height) return S_OK;
            if (!factory_) {
                auto result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
                if (FAILED(result)) return result;
            }
            bitmap_.Reset();
            auto result = factory_->CreateBitmap(width, height, GUID_WICPixelFormat32bppBGRA,
                WICBitmapCacheOnLoad, &bitmap_);
            if (SUCCEEDED(result)) { width_ = width; height_ = height; }
            return result;
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap_;
        UINT width_{}, height_{};
    };
}
