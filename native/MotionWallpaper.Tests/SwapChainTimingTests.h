#pragma once

#include "../MotionWallpaper.Common/MediaProbe.h"
#include "../MotionWallpaper.Common/UniqueHandle.h"
#include "../MotionWallpaper.Renderer/OutputFramePolicy.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace motion::tests
{
    template <typename Require>
    inline void output_progress_isolated_retry_and_freeze_barrier(Require require)
    {
        using motion::renderer::OutputFrameProgress;
        std::array<OutputFrameProgress, 2> outputs{};
        auto ready = [&](bool freeze) { return outputs[0].Ready(freeze) && outputs[1].Ready(freeze); };
        require(!ready(false) && !outputs[0].NeedsFrame(0, false),
            "an empty decoder frame was eligible for rendering or startup acknowledgement");

        // The first screen accepts frame 1 while the second is back-pressured.
        // Only successful Present advances progress; a retry does not.
        outputs[0].Presented(1, false);
        require(!ready(false) && !outputs[0].NeedsFrame(1, false) && outputs[1].NeedsFrame(1, false),
            "one output acknowledged startup or consumed the other output's frame");
        for (uint64_t serial = 2; serial <= 8; ++serial) {
            require(outputs[0].NeedsFrame(serial, false), "a stalled peer blocked the ready output");
            outputs[0].Presented(serial, false);
        }
        require(outputs[0].presentedSerial == 8 && outputs[1].presentedSerial == 0 && !ready(false),
            "a slow output corrupted independent presentation progress");
        outputs[1].Presented(8, false);
        require(ready(false) && !outputs[1].NeedsFrame(8, false),
            "a delayed output could not catch up when the decoder had no newer frame");

        // A frozen frame is new work even if the same decoded serial was shown.
        for (auto& output : outputs) output.BeginFreeze();
        require(!ready(true) && outputs[0].NeedsFrame(8, true) && outputs[1].NeedsFrame(8, true),
            "freeze reused ordinary playback progress without capturing every output");
        outputs[0].Presented(8, true);
        require(!ready(true) && !outputs[0].NeedsFrame(9, true) && outputs[1].NeedsFrame(9, true),
            "freeze was acknowledged early or repeatedly captured an already-frozen screen");
        outputs[1].Presented(9, false); // A pre-freeze pending Present finally succeeds.
        require(!ready(true) && outputs[1].NeedsFrame(9, true),
            "a pre-freeze pending frame satisfied the freeze barrier without a capture");
        outputs[1].Presented(9, true);
        require(ready(true), "all successful captures did not satisfy the freeze barrier");
        require(outputs[0].NeedsFrame(10, false) && outputs[1].NeedsFrame(10, false),
            "frozen outputs could not resume on the next decoded frame");
        for (auto& output : outputs) output.BeginFreeze();
        require(!ready(true), "a second freeze inherited the previous freeze acknowledgement");

        // Serials, rather than source timestamps, distinguish a loop restart.
        outputs[0].Presented(100, false);
        require(outputs[0].NeedsFrame(101, false), "loop restart was mistaken for a duplicate timestamp");
    }

    namespace swap_chain_test_detail
    {
        struct HardwareUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };

        inline void check(HRESULT result, char const* operation)
        {
            if (FAILED(result)) {
                char value[128]{};
                std::snprintf(value, sizeof(value), "%s 0x%08lX", operation, static_cast<unsigned long>(result));
                throw std::runtime_error(value);
            }
        }

        // No HWND, DComp target, display-mode change, or visible content is
        // created. This verifies the real DXGI contract, not monitor cadence.
        inline void composition_swap_chain_contract()
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            constexpr D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            auto deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &device, nullptr, &context);
            if (FAILED(deviceResult)) {
                char value[64]{};
                std::snprintf(value, sizeof(value), "no-physical-d3d11 0x%08lX",
                    static_cast<unsigned long>(deviceResult));
                throw HardwareUnavailable(value);
            }
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            check(device.As(&dxgiDevice), "query-dxgi-device");
            check(dxgiDevice->GetAdapter(&adapter), "get-adapter");
            check(adapter->GetParent(IID_PPV_ARGS(&factory)), "get-factory");

            struct Output {
                ComPtr<IDXGISwapChain2> chain;
                motion::unique_handle latency;
                bool prepared{};
                bool firstPresentChecked{};
            };
            std::array<Output, 2> outputs;
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = description.Height = 32;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            for (auto& output : outputs) {
                ComPtr<IDXGISwapChain1> chain;
                check(factory->CreateSwapChainForComposition(device.Get(), &description, nullptr, &chain),
                    "create-waitable-composition-chain");
                check(chain.As(&output.chain), "query-chain2");
                DXGI_SWAP_CHAIN_DESC1 actual{};
                check(output.chain->GetDesc1(&actual), "get-description");
                if (!(actual.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
                    throw std::runtime_error("created composition chain lost its waitable flag");
                check(output.chain->SetMaximumFrameLatency(1), "set-maximum-latency");
                UINT latency{};
                check(output.chain->GetMaximumFrameLatency(&latency), "get-maximum-latency");
                if (latency != 1) throw std::runtime_error("swap chain did not retain maximum latency 1");
                output.latency.reset(output.chain->GetFrameLatencyWaitableObject());
                if (!output.latency) throw std::runtime_error("waitable composition chain returned a null handle");
            }

            // Poll both outputs with timeout zero; no display can block the
            // next output. The parent process separately enforces a deadline
            // around device creation, driver calls, and destruction.
            ULONGLONG deadline = GetTickCount64() + 2'000;
            while (!outputs[0].firstPresentChecked || !outputs[1].firstPresentChecked) {
                for (auto& output : outputs) {
                    if (output.firstPresentChecked) continue;
                    if (!output.prepared) {
                        DWORD result = WaitForSingleObject(output.latency.get(), 0);
                        if (result == WAIT_TIMEOUT) continue;
                        if (result != WAIT_OBJECT_0) throw std::runtime_error("invalid latency wait result");
                        ComPtr<ID3D11Texture2D> buffer;
                        ComPtr<ID3D11RenderTargetView> target;
                        check(output.chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "get-buffer");
                        check(device->CreateRenderTargetView(buffer.Get(), nullptr, &target), "create-target");
                        constexpr float black[4]{ 0, 0, 0, 1 };
                        context->ClearRenderTargetView(target.Get(), black);
                        output.prepared = true;
                    }
                    DXGI_PRESENT_PARAMETERS parameters{};
                    HRESULT result = output.chain->Present1(1, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
                    if (result == DXGI_ERROR_WAS_STILL_DRAWING) continue;
                    check(result, "nonblocking-present");
                    // Without a composition target there is intentionally no
                    // visible output. OCCLUDED validates the call contract but
                    // must never be described as a scanned-out video frame.
                    if (result != S_OK && result != DXGI_STATUS_OCCLUDED) {
                        char value[128]{};
                        std::snprintf(value, sizeof(value), "unattached composition present status 0x%08lX",
                            static_cast<unsigned long>(result));
                        throw std::runtime_error(value);
                    }
                    output.firstPresentChecked = true;
                }
                if (GetTickCount64() >= deadline) throw std::runtime_error("first-frame contract exceeded its deadline");
                if (!outputs[0].firstPresentChecked || !outputs[1].firstPresentChecked) Sleep(1);
            }

            for (auto& output : outputs) {
                HANDLE handle = output.latency.get();
                output.latency.reset();
                DWORD flags{};
                if (GetHandleInformation(handle, &flags) || GetLastError() != ERROR_INVALID_HANDLE)
                    throw std::runtime_error("latency handle was not closed");
                output.chain.Reset();
            }
            context->ClearState();
            context->Flush();
        }
    }

    // Dispatch before COM initialization in the test runner. Driver work must
    // remain in the bounded child, so a driver hang cannot hang all tests.
    inline std::optional<int> handle_swap_chain_timing_test_command(int argc, wchar_t** argv)
    {
        if (argc != 2 || std::wstring_view(argv[1]) != L"--swap-chain-timing-test-child") return {};
        try {
            swap_chain_test_detail::composition_swap_chain_contract();
            std::puts("swap-chain-v1 hardware 2 headless-present-calls waitable closed");
        } catch (swap_chain_test_detail::HardwareUnavailable const& error) {
            std::printf("SKIP %s\n", error.what());
        } catch (std::exception const& error) {
            std::printf("FAIL %s\n", error.what());
        }
        return 0;
    }

    template <typename Require>
    inline void real_composition_swap_chain_contract_is_bounded(Require require)
    {
        std::wstring executable(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length && length < executable.size(), "could not locate the swap-chain test runner");
        executable.resize(length);
        auto result = motion::media_tool_detail::run_bounded(executable,
            { L"--swap-chain-timing-test-child" }, 12'000);
        require(result.has_value(), "swap-chain child timed out, crashed, or could not be launched");
        if (result->starts_with("SKIP no-physical-d3d11 ")) {
            // CI runners can have no physical D3D11 device. This is the only
            // skip condition; driver/swap-chain failures remain test failures.
            std::printf("[SKIP] real composition contract: %s", result->c_str());
            return;
        }
        // CRT text-mode stdout translates the final newline on Windows.
        require(*result == "swap-chain-v1 hardware 2 headless-present-calls waitable closed\r\n" ||
            *result == "swap-chain-v1 hardware 2 headless-present-calls waitable closed\n", result->c_str());
    }
}
