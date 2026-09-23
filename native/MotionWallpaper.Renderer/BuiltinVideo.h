#pragma once
#include <windows.h>
#include <d3d11.h>
#include <mfmediaengine.h>
#include <memory>
#include <string>

namespace motion::renderer {
    // One bounded decoder/clock per renderer route; every output samples the
    // same current frame. Optional libraries are loaded only on MF failure.
    class BuiltinVideo final {
    public:
        BuiltinVideo();
        ~BuiltinVideo();
        BuiltinVideo(BuiltinVideo const&) = delete;
        BuiltinVideo& operator=(BuiltinVideo const&) = delete;
        bool Start(std::wstring const& path, ID3D11Device* device, bool hardware,
            bool allowSoftware, unsigned displayRate, double resumeSeconds = 0);
        HRESULT Tick(LONGLONG* timestamp);
        HRESULT Size(DWORD* width, DWORD* height) const;
        HRESULT Draw(ID3D11Texture2D* destination, MFVideoNormalizedRect const& crop);
        void Play();
        void Pause();
        double CurrentTime() const;
        bool Hardware() const;
        std::string FailureReason() const;
        void Shutdown();
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
