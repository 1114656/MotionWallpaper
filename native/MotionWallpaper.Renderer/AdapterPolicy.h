#pragma once

#include <cstdint>
#include <limits>

namespace motion::renderer
{
    struct AdapterCandidate
    {
        uint32_t enumerationOrder{};
        int displayRank{};
        uint64_t dedicatedVideoMemory{};
    };

    // DXGI 1.6 already returns adapters in the requested GPU-preference order.
    // Keep that order for heavy workloads so a muxless discrete GPU is not
    // pushed behind the integrated display adapter merely because it owns no
    // physical output. On older DXGI versions, dedicated memory is the most
    // useful vendor-neutral fallback signal. Light workloads still prefer the
    // display-attached adapter to avoid cross-adapter copies and waking a dGPU.
    [[nodiscard]] constexpr bool adapter_candidate_precedes(
        AdapterCandidate const& left,
        AdapterCandidate const& right,
        bool preferHighPerformance,
        bool gpuPreferenceOrderAvailable) noexcept
    {
        if (preferHighPerformance) {
            if (gpuPreferenceOrderAvailable &&
                left.enumerationOrder != right.enumerationOrder) {
                return left.enumerationOrder < right.enumerationOrder;
            }
            if (!gpuPreferenceOrderAvailable &&
                left.dedicatedVideoMemory != right.dedicatedVideoMemory) {
                return left.dedicatedVideoMemory > right.dedicatedVideoMemory;
            }
        }
        if (left.displayRank != right.displayRank) {
            return left.displayRank < right.displayRank;
        }
        return left.enumerationOrder < right.enumerationOrder;
    }

    // Above roughly 4K60, the cost of decoding and copying a full-resolution
    // frame every refresh can saturate an integrated GPU. Preserve native
    // resolution and frame rate by preferring the high-performance adapter;
    // lighter media stays on the display adapter to minimize power use.
    [[nodiscard]] constexpr bool prefer_high_performance_adapter(
        uint32_t width,
        uint32_t height,
        uint32_t frameRateNumerator,
        uint32_t frameRateDenominator,
        uint64_t aggregateOutputPixels = 0) noexcept
    {
        if (!width || !height || !frameRateNumerator || !frameRateDenominator) return false;
        constexpr uint64_t highWorkloadPixelsPerSecond =
            static_cast<uint64_t>(3840) * 2160 * 60;
        auto saturatedMultiply = [](uint64_t left, uint64_t right) constexpr noexcept {
            return left && right > (std::numeric_limits<uint64_t>::max)() / left
                ? (std::numeric_limits<uint64_t>::max)()
                : left * right;
        };
        auto threshold = saturatedMultiply(highWorkloadPixelsPerSecond, frameRateDenominator);
        auto decodeWork = saturatedMultiply(
            saturatedMultiply(static_cast<uint64_t>(width), height), frameRateNumerator);
        auto outputWork = saturatedMultiply(aggregateOutputPixels, frameRateNumerator);
        // Compare the rational workload without dividing. Saturation is
        // deliberately conservative: malformed or extreme dimensions remain
        // classified as heavy instead of wrapping around to a light load.
        return decodeWork >= threshold || outputWork >= threshold;
    }
}
