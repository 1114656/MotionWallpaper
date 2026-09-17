#include "SceneProfiles.h"

#include <algorithm>
#include <array>
#include <limits>

namespace motion
{
    namespace
    {
        constexpr std::array<std::string_view, 5> validKinds{
            "custom", "work", "night", "battery", "presentation"
        };

        bool valid_selection(std::string const& groupId, std::string const& mediaId) noexcept
        {
            return groupId.empty() && mediaId.empty() ||
                valid_id(groupId) && valid_id(mediaId);
        }

        bool valid_performance_mode(std::string const& value) noexcept
        {
            return value == "balanced" || value == "original" ||
                value == "power-saver";
        }

        bool valid_display_mode(std::string const& value) noexcept
        {
            return value == "independent" || value == "primary";
        }

        bool valid_kind(std::string const& value) noexcept
        {
            return std::find(validKinds.begin(), validKinds.end(), value) != validKinds.end();
        }

        bool valid_display_id(std::string const& value) noexcept
        {
            return !value.empty() && value.size() <= 512 &&
                std::all_of(value.begin(), value.end(), [](unsigned char character) {
                    return character >= 0x20 && character != 0x7f;
                });
        }

        bool valid_assignment(DisplayAssignment const& value) noexcept
        {
            return valid_display_id(value.displayId) &&
                valid_id(value.groupId) && valid_id(value.mediaId);
        }

        bool valid_activation(SceneActivationRule const& value) noexcept
        {
            if (value.trigger != "manual" && value.trigger != "time-range" &&
                value.trigger != "battery" && value.trigger != "presentation") {
                return false;
            }
            return value.startMinute >= 0 && value.startMinute < 24 * 60 &&
                value.endMinute >= 0 && value.endMinute < 24 * 60 &&
                value.priority >= -1000 && value.priority <= 1000;
        }

        bool valid_scene(SceneProfile const& value) noexcept
        {
            if (!valid_scene_profile_id(value.id) || value.name.empty() || value.name.size() > 64 ||
                !std::all_of(value.name.begin(), value.name.end(), [](wchar_t character) {
                    return character >= 0x20 && character != 0x7f;
                }) ||
                !valid_kind(value.kind) ||
                !valid_selection(value.defaultGroupId, value.defaultMediaId) ||
                !valid_performance_mode(value.performanceMode) ||
                !valid_display_mode(value.displayMode) ||
                !valid_activation(value.activation) ||
                value.displayAssignments.size() > 32) {
                return false;
            }
            for (size_t index = 0; index < value.displayAssignments.size(); ++index) {
                if (!valid_assignment(value.displayAssignments[index])) return false;
                auto duplicate = std::find_if(value.displayAssignments.begin(),
                    value.displayAssignments.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](auto const& existing) {
                        return existing.displayId == value.displayAssignments[index].displayId;
                    });
                if (duplicate != value.displayAssignments.begin() +
                    static_cast<std::ptrdiff_t>(index)) return false;
            }
            return true;
        }

        SceneProfile make_builtin(Settings const& settings, std::string id,
            std::wstring name, std::string kind, std::string performanceMode,
            bool playbackEnabled, bool screensaverEnabled,
            SceneActivationRule activation)
        {
            SceneProfile scene;
            scene.id = std::move(id);
            scene.name = std::move(name);
            scene.kind = std::move(kind);
            scene.defaultGroupId = settings.selectedGroupId;
            scene.defaultMediaId = settings.selectedMediaId;
            scene.performanceMode = std::move(performanceMode);
            scene.displayMode = settings.displayMode;
            scene.activePlaybackEnabled = playbackEnabled;
            scene.screensaverEnabled = screensaverEnabled;
            scene.displayAssignments = settings.displayAssignments;
            scene.activation = std::move(activation);
            return scene;
        }

        bool time_range_matches(int minute, int start, int end) noexcept
        {
            if (start == end) return true;
            return start < end
                ? minute >= start && minute < end
                : minute >= start || minute < end;
        }
    }

    void ensure_builtin_scene_profiles(Settings& settings)
    {
        auto add = [&](SceneProfile scene) {
            if (settings.scenes.size() < 64 && !find_scene_profile(settings, scene.id)) {
                settings.scenes.push_back(std::move(scene));
            }
        };
        add(make_builtin(settings, std::string(work_scene_id), L"工作", "work", "balanced", true, true,
            { "manual", false, 0, 0, 0 }));
        add(make_builtin(settings, std::string(night_scene_id), L"夜间", "night", "power-saver", true, true,
            { "time-range", false, 20 * 60, 7 * 60, 100 }));
        add(make_builtin(settings, std::string(battery_scene_id), L"电池", "battery", "power-saver", false, false,
            { "battery", false, 0, 0, 200 }));
        add(make_builtin(settings, std::string(presentation_scene_id), L"投屏", "presentation", "balanced", false, false,
            { "presentation", false, 0, 0, 300 }));
    }

    bool valid_scene_profile_id(std::string const& value) noexcept
    {
        return !value.empty() && value.size() <= 64 &&
            std::all_of(value.begin(), value.end(), [](unsigned char character) {
                return character >= 'a' && character <= 'z' ||
                    character >= '0' && character <= '9' || character == '-';
            });
    }

    bool valid_scene_profile(SceneProfile const& value) noexcept
    {
        return valid_scene(value);
    }

    SceneProfile* find_scene_profile(Settings& settings, std::string const& sceneId) noexcept
    {
        auto found = std::find_if(settings.scenes.begin(), settings.scenes.end(),
            [&](auto const& scene) { return scene.id == sceneId; });
        return found == settings.scenes.end() ? nullptr : &*found;
    }

    SceneProfile const* find_scene_profile(
        Settings const& settings, std::string const& sceneId) noexcept
    {
        auto found = std::find_if(settings.scenes.begin(), settings.scenes.end(),
            [&](auto const& scene) { return scene.id == sceneId; });
        return found == settings.scenes.end() ? nullptr : &*found;
    }

    bool capture_scene_profile(
        Settings const& current, SceneProfile& destination) noexcept
    {
        try {
            auto candidate = destination;
            candidate.defaultGroupId = current.selectedGroupId;
            candidate.defaultMediaId = current.selectedMediaId;
            candidate.performanceMode = current.performanceMode;
            candidate.displayMode = current.displayMode;
            candidate.activePlaybackEnabled = current.activePlaybackEnabled;
            candidate.screensaverEnabled = current.screensaverEnabled;
            candidate.displayAssignments = current.displayAssignments;
            if (!valid_scene(candidate)) return false;
            destination = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool scene_profile_matches_settings(
        SceneProfile const& scene, Settings const& settings) noexcept
    {
        try {
            auto assignmentsMatch = [](auto const& left, auto const& right) {
                if (left.size() != right.size()) return false;
                return std::all_of(left.begin(), left.end(), [&](auto const& assignment) {
                    return std::any_of(right.begin(), right.end(), [&](auto const& candidate) {
                        return assignment.displayId == candidate.displayId &&
                            assignment.groupId == candidate.groupId &&
                            assignment.mediaId == candidate.mediaId;
                    });
                });
            };
            return valid_scene(scene) &&
                scene.defaultGroupId == settings.selectedGroupId &&
                scene.defaultMediaId == settings.selectedMediaId &&
                scene.performanceMode == settings.performanceMode &&
                scene.displayMode == settings.displayMode &&
                scene.activePlaybackEnabled == settings.activePlaybackEnabled &&
                scene.screensaverEnabled == settings.screensaverEnabled &&
                assignmentsMatch(scene.displayAssignments, settings.displayAssignments);
        } catch (...) {
            return false;
        }
    }

    bool apply_scene_profile(Settings& settings, std::string const& sceneId) noexcept
    {
        try {
            auto scene = find_scene_profile(settings, sceneId);
            if (!scene || !valid_scene(*scene)) return false;
            auto candidate = settings;
            candidate.selectedGroupId = scene->defaultGroupId;
            candidate.selectedMediaId = scene->defaultMediaId;
            candidate.performanceMode = scene->performanceMode;
            candidate.displayMode = scene->displayMode;
            candidate.activePlaybackEnabled = scene->activePlaybackEnabled;
            candidate.screensaverEnabled = scene->screensaverEnabled;
            candidate.displayAssignments = scene->displayAssignments;
            candidate.activeSceneId = scene->id;
            settings = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::optional<std::string> automatic_scene_for_context(
        Settings const& settings, SceneActivationContext const& context) noexcept
    {
        try {
            if (context.localMinuteOfDay < 0 || context.localMinuteOfDay >= 24 * 60) {
                return std::nullopt;
            }
            SceneProfile const* best{};
            for (auto const& scene : settings.scenes) {
                if (!valid_scene(scene) || !scene.activation.enabled ||
                    scene.activation.trigger == "manual") continue;
                bool matches = scene.activation.trigger == "time-range"
                    ? time_range_matches(context.localMinuteOfDay,
                        scene.activation.startMinute, scene.activation.endMinute)
                    : scene.activation.trigger == "battery"
                        ? context.onBattery
                        : scene.activation.trigger == "presentation" &&
                            context.presentationActive;
                if (matches && (!best || scene.activation.priority > best->activation.priority)) {
                    best = &scene;
                }
            }
            return best ? std::optional<std::string>(best->id) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    DisplayLayoutModel build_display_layout_model(
        std::vector<DisplayTarget> const& displays,
        std::vector<DisplayAssignment> const& assignments)
    {
        DisplayLayoutModel model;
        if (displays.empty()) return model;
        model.virtualBounds = {
            (std::numeric_limits<LONG>::max)(),
            (std::numeric_limits<LONG>::max)(),
            (std::numeric_limits<LONG>::min)(),
            (std::numeric_limits<LONG>::min)()
        };
        model.displays.reserve(displays.size());
        for (auto const& display : displays) {
            DisplayLayoutItem item;
            item.displayId = display.id;
            item.deviceName = display.deviceName;
            item.friendlyName = display.friendlyName;
            item.bounds = display.bounds;
            item.primary = display.primary;
            auto assignment = std::find_if(assignments.begin(), assignments.end(),
                [&](auto const& value) { return value.displayId == display.id; });
            if (assignment != assignments.end()) item.assignment = *assignment;
            model.virtualBounds.left = (std::min)(model.virtualBounds.left, item.bounds.left);
            model.virtualBounds.top = (std::min)(model.virtualBounds.top, item.bounds.top);
            model.virtualBounds.right = (std::max)(model.virtualBounds.right, item.bounds.right);
            model.virtualBounds.bottom = (std::max)(model.virtualBounds.bottom, item.bounds.bottom);
            model.displays.push_back(std::move(item));
        }
        return model;
    }
}
