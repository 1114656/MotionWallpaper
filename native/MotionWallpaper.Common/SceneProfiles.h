#pragma once

#include "Common.h"
#include "DisplayTopology.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace motion
{
    inline constexpr std::string_view work_scene_id = "work";
    inline constexpr std::string_view night_scene_id = "night";
    inline constexpr std::string_view battery_scene_id = "battery";
    inline constexpr std::string_view presentation_scene_id = "presentation";

    struct SceneActivationContext
    {
        int localMinuteOfDay{};
        bool onBattery{};
        bool presentationActive{};
    };

    struct DisplayLayoutItem
    {
        std::string displayId;
        std::wstring deviceName;
        std::wstring friendlyName;
        RECT bounds{};
        bool primary{};
        std::optional<DisplayAssignment> assignment;
    };

    struct DisplayLayoutModel
    {
        RECT virtualBounds{};
        std::vector<DisplayLayoutItem> displays;
    };

    // Adds any missing built-in scenes without overwriting user customizations.
    // The initial wallpaper/layout is captured so applying a new preset is safe.
    void ensure_builtin_scene_profiles(Settings& settings);

    [[nodiscard]] bool valid_scene_profile_id(std::string const& value) noexcept;
    [[nodiscard]] bool valid_scene_profile(SceneProfile const& value) noexcept;

    [[nodiscard]] SceneProfile* find_scene_profile(
        Settings& settings, std::string const& sceneId) noexcept;
    [[nodiscard]] SceneProfile const* find_scene_profile(
        Settings const& settings, std::string const& sceneId) noexcept;

    // Captures the current wallpaper/layout and playback preferences into an
    // existing scene. Identity, display name, kind, and activation stay intact.
    [[nodiscard]] bool capture_scene_profile(
        Settings const& current, SceneProfile& destination) noexcept;

    // True only while the live settings still match the saved scene snapshot.
    // This lets the UI distinguish an active scene from one the user has
    // manually changed after applying it.
    [[nodiscard]] bool scene_profile_matches_settings(
        SceneProfile const& scene, Settings const& settings) noexcept;

    // Applies a scene as one settings mutation. No partial changes are made if
    // the scene is invalid or cannot be found.
    [[nodiscard]] bool apply_scene_profile(
        Settings& settings, std::string const& sceneId) noexcept;

    // Chooses the highest-priority enabled automatic scene. Manual scenes never
    // match this function and remain available through apply_scene_profile.
    [[nodiscard]] std::optional<std::string> automatic_scene_for_context(
        Settings const& settings, SceneActivationContext const& context) noexcept;

    // Builds a stable, renderer-independent layout model for a diagram. The
    // rectangle retains real virtual-desktop coordinates, including negatives.
    [[nodiscard]] DisplayLayoutModel build_display_layout_model(
        std::vector<DisplayTarget> const& displays,
        std::vector<DisplayAssignment> const& assignments);
}
