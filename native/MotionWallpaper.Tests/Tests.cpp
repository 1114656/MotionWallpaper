#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/SceneProfiles.h"
#include "../MotionWallpaper.Agent/RandomSelectionPolicy.h"
#include "../MotionWallpaper.Agent/RuntimePolicy.h"
#include "../MotionWallpaper.Agent/CoveragePolicy.h"
#include "../MotionWallpaper.Agent/IdlePolicy.h"
#include "../MotionWallpaper.Agent/PlaybackCapabilityPolicy.h"
#include "../MotionWallpaper.Agent/RuntimeEventPolicy.h"
#include "../MotionWallpaper.Agent/RuntimeStatusPolicy.h"
#include "../MotionWallpaper.Agent/SharedRendererPolicy.h"
#include "../MotionWallpaper.Agent/TrayControlPolicy.h"
#include "../MotionWallpaper.Agent/VideoOptimizer.h"
#include "../MotionWallpaper.Agent/VideoVariantPolicy.h"
#include "../MotionWallpaper.Agent/VideoTranscoder.h"
#include "../MotionWallpaper.Renderer/ResidencyPolicy.h"
#include "../MotionWallpaper.Renderer/FrameTiming.h"
#include "../MotionWallpaper.Renderer/SoftwareFramePolicy.h"
#include "../MotionWallpaper.Renderer/DecodePolicy.h"
#include "../MotionWallpaper.Renderer/FrameScheduler.h"
#include "../MotionWallpaper.Renderer/AdapterPolicy.h"
#include "../MotionWallpaper.Renderer/DesktopHostPolicy.h"
#include "../MotionWallpaper.Renderer/TransitionPolicy.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"
#include "../MotionWallpaper.App/LibraryMigration.h"
#include "../MotionWallpaper.App/MediaLibrary.h"
#include "LibraryBackupTests.h"

#include <winrt/base.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void require(bool condition, char const* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    constexpr char testLibraryId[] = "dddddddd-dddd-dddd-dddd-dddddddddddd";
    constexpr char replacementLibraryId[] = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";

    void create_owned_library_root(fs::path const& root,
        char const* ownershipId = testLibraryId)
    {
        fs::create_directories(root / L"Groups");
        std::ofstream(root / motion::media_library_ownership_marker_name, std::ios::binary)
            << motion::media_library_ownership_marker_prefix
            << ownershipId << '\n';
    }

    void settings_round_trip_clears_empty_values(fs::path const& root)
    {
        auto path = root / L"settings.json";
        motion::Settings settings;
        settings.selectedGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        settings.selectedMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        settings.randomGroupId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        settings.randomIntervalMinutes = 15;
        settings.startWithWindows = true;
        settings.autoLockEnabled = false;
        settings.autoLockTimeoutSeconds = 600;
        settings.displayOffAfterLockEnabled = true;
        settings.displayOffAfterLockDelaySeconds = 30;
        settings.performanceMode = "power-saver";
        settings.mediaLibraryPath = (root / L"custom-wallpapers").wstring();
        create_owned_library_root(settings.mediaLibraryPath);
        settings.mediaLibraryId = testLibraryId;
        settings.displayMode = "primary";
        settings.displayAssignments.push_back({ "MONITOR\\TEST\\1", settings.selectedGroupId, settings.selectedMediaId });
        motion::save_settings(path, settings);
        {
            std::ifstream input(path, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            require(json.find("showScreensaverClock") == std::string::npos,
                "removed screen saver clock setting survived serialization");
        }
        auto populated = motion::load_settings(path);
        if (!populated) throw std::runtime_error("saved settings could not be loaded");
        require(populated->selectedMediaId == settings.selectedMediaId, "non-empty selection did not round-trip");
        require(populated->randomIntervalMinutes == 15 && populated->startWithWindows, "new settings did not round-trip");
        require(!populated->autoLockEnabled && populated->autoLockTimeoutSeconds == 600,
            "automatic-lock settings did not round-trip");
        require(populated->displayOffAfterLockEnabled && populated->displayOffAfterLockDelaySeconds == 30,
            "post-lock display-off settings did not round-trip");
        require(populated->performanceMode == "power-saver", "wallpaper performance mode did not round-trip");
        require(populated->mediaLibraryPath == fs::path(settings.mediaLibraryPath).lexically_normal().wstring(),
            "custom media-library path did not round-trip");
        require(populated->mediaLibraryId == settings.mediaLibraryId,
            "custom media-library ownership ID did not round-trip");
        require(populated->displayMode == "primary", "display mode did not round-trip");
        require(populated->displayAssignments.size() == 1 && populated->displayAssignments.front().displayId == "MONITOR\\TEST\\1",
            "per-display wallpaper assignment did not round-trip");

        settings.selectedGroupId.clear();
        settings.selectedMediaId.clear();
        settings.randomGroupId.clear();
        settings.displayAssignments.clear();
        motion::save_settings(path, settings);
        auto cleared = motion::load_settings(path);
        require(cleared.has_value(), "cleared settings could not be loaded");
        require(cleared->version == motion::settings_schema_version, "settings schema version did not round-trip");
        require(cleared->selectedGroupId.empty(), "selected group survived clearing");
        require(cleared->selectedMediaId.empty(), "selected media survived clearing");
        require(cleared->randomGroupId.empty(), "random group survived clearing");
        require(cleared->displayAssignments.empty(), "display assignment survived clearing");
    }

    void application_data_location_preserves_portable_and_legacy_libraries(fs::path const& root)
    {
        auto applicationRoot = root / L"application";
        auto localRoot = root / L"local";
        fs::create_directories(applicationRoot);
        require(motion::ffmpeg_executable_path(applicationRoot) ==
            applicationRoot / L"Tools" / L"ffmpeg" / L"ffmpeg.exe",
            "installed FFmpeg path was detached from the application directory");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == localRoot / L"MotionWallpaper",
            "fresh installed app did not use LocalAppData");
        require(motion::wallpaper_library_directory(applicationRoot) == applicationRoot / L"Wallpapers",
            "default media library was detached from the installed App directory");
        auto customLibrary = root / L"relocated" / L"Wallpapers";
        require(motion::wallpaper_library_directory(applicationRoot, customLibrary.wstring()) == customLibrary,
            "custom media-library location was ignored");
        bool relativeRejected{};
        try { (void)motion::wallpaper_library_directory(applicationRoot, L"relative\\Wallpapers"); }
        catch (...) { relativeRejected = true; }
        require(relativeRejected,
            "an invalid configured media-library path silently fell back to a writable default");
        auto identityPath = fs::absolute(root / L"Case-Identity" / L"Folder");
        auto identityAlias = identityPath.parent_path() / L"." / L"folder";
        require(motion::same_filesystem_path(identityPath, identityAlias) &&
            motion::filesystem_path_is_nested(identityPath.parent_path(), identityAlias),
            "Windows path identity was case-sensitive or failed to normalize aliases");

        std::ofstream(applicationRoot / L"portable.mode") << "portable\n";
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "portable marker did not keep data beside the executable");

        auto legacyRoot = localRoot / L"MotionWallpaper";
        fs::create_directories(legacyRoot);
        std::ofstream(applicationRoot / motion::legacy_data_fallback_marker_name)
            << "MotionWallpaper.LegacyFallback/v1\n";
        fs::create_directories(applicationRoot / L"Config");
        fs::create_directories(applicationRoot / L"Wallpapers");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == legacyRoot,
            "an installer fallback marker did not override partial portable data");
        fs::remove(applicationRoot / motion::legacy_data_fallback_marker_name);
        fs::remove_all(applicationRoot / L"Config");
        fs::remove_all(applicationRoot / L"Wallpapers");
        fs::remove_all(legacyRoot);

        std::ofstream(applicationRoot / motion::legacy_data_fallback_marker_name)
            << "MotionWallpaper.LegacyFallback/v1\n";
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "a fallback marker selected a missing LocalAppData legacy root");
        fs::remove(applicationRoot / motion::legacy_data_fallback_marker_name);
        fs::remove(applicationRoot / L"portable.mode");

        std::ofstream(applicationRoot / motion::legacy_data_conflict_marker_name)
            << "MotionWallpaper.LegacyConflict/v1\n";
        require(motion::legacy_data_conflict_present(applicationRoot),
            "an installer data-conflict marker was not recognized");
        fs::remove(applicationRoot / motion::legacy_data_conflict_marker_name);
        fs::create_directories(applicationRoot / motion::legacy_data_conflict_marker_name);
        require(!motion::legacy_data_conflict_present(applicationRoot),
            "a directory was accepted as the installer data-conflict marker");
        fs::remove(applicationRoot / motion::legacy_data_conflict_marker_name);

        fs::create_directories(applicationRoot / L"Config");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "legacy configuration was detached from its executable");
        fs::remove_all(applicationRoot / L"Config");

        fs::create_directories(applicationRoot / L"Wallpapers");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "legacy wallpaper library was detached from its executable");
    }

    void scene_profiles_and_layout_round_trip(fs::path const& root)
    {
        constexpr char groupA[] = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        constexpr char mediaA[] = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        constexpr char mediaB[] = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        motion::Settings settings;
        settings.selectedGroupId = groupA;
        settings.selectedMediaId = mediaA;
        settings.displayMode = "primary";
        settings.displayAssignments.push_back({ "display-right", groupA, mediaB });
        motion::ensure_builtin_scene_profiles(settings);
        require(settings.scenes.size() == 4,
            "the four built-in scene profiles were not added exactly once");

        auto work = motion::find_scene_profile(settings, std::string(motion::work_scene_id));
        require(work && motion::capture_scene_profile(settings, *work),
            "the current wallpaper/layout could not be captured into a scene");
        settings.selectedMediaId = mediaB;
        settings.performanceMode = "power-saver";
        settings.displayMode = "independent";
        settings.activePlaybackEnabled = false;
        settings.displayAssignments.clear();
        require(motion::apply_scene_profile(settings, std::string(motion::work_scene_id)) &&
            settings.selectedMediaId == mediaA && settings.performanceMode == "balanced" &&
            settings.displayMode == "primary" && settings.activePlaybackEnabled &&
            settings.displayAssignments.size() == 1 &&
            settings.activeSceneId == motion::work_scene_id,
            "applying a scene was partial or did not restore its display assignments");

        auto night = motion::find_scene_profile(settings, std::string(motion::night_scene_id));
        auto battery = motion::find_scene_profile(settings, std::string(motion::battery_scene_id));
        auto presentation = motion::find_scene_profile(
            settings, std::string(motion::presentation_scene_id));
        require(night && battery && presentation,
            "one or more automatic scene presets are missing");
        night->activation.enabled = true;
        battery->activation.enabled = true;
        presentation->activation.enabled = true;
        require(motion::automatic_scene_for_context(settings, { 21 * 60, true, true }) ==
                std::optional<std::string>(motion::presentation_scene_id) &&
            motion::automatic_scene_for_context(settings, { 21 * 60, true, false }) ==
                std::optional<std::string>(motion::battery_scene_id) &&
            motion::automatic_scene_for_context(settings, { 21 * 60, false, false }) ==
                std::optional<std::string>(motion::night_scene_id),
            "automatic scene priority does not prefer presentation, battery, then time range");

        std::vector<motion::DisplayTarget> displays{
            { "display-left", L"DISPLAY1", L"Left", { -1920, 0, 0, 1080 }, 60, false },
            { "display-right", L"DISPLAY2", L"Right", { 0, 0, 2560, 1440 }, 120, true }
        };
        auto layout = motion::build_display_layout_model(displays, settings.displayAssignments);
        require(layout.displays.size() == 2 && layout.virtualBounds.left == -1920 &&
            layout.virtualBounds.right == 2560 && layout.virtualBounds.bottom == 1440 &&
            !layout.displays[0].assignment && layout.displays[1].assignment &&
            layout.displays[1].assignment->mediaId == mediaB,
            "display layout model lost negative coordinates, primary bounds, or assignment data");

        settings.optimizationStorageQuotaBytes = 5ULL * 1024 * 1024 * 1024;
        auto path = root / L"scene-settings.json";
        motion::save_settings(path, settings);
        auto loaded = motion::load_settings(path);
        require(loaded && loaded->optimizationStorageQuotaBytes ==
                settings.optimizationStorageQuotaBytes &&
            loaded->activeSceneId == motion::work_scene_id &&
            loaded->scenes.size() == 4 &&
            motion::find_scene_profile(*loaded, std::string(motion::battery_scene_id)),
            "scene profiles or optimization quota did not survive settings round-trip");
    }

    void legacy_settings_are_migrated(fs::path const& root)
    {
        auto path = root / L"legacy-settings.json";
        std::ofstream(path) << R"({"version":6,"selectedVideoId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","displayMode":"span","autoLockEnabled":false,"lockTimeoutSeconds":60})";
        auto settings = motion::load_settings(path);
        if (!settings) throw std::runtime_error("legacy settings could not be loaded");
        require(settings->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "legacy media key was not loaded");
        require(settings->version == motion::settings_schema_version, "legacy settings were not migrated to the current schema");
        require(settings->displayMode == "independent", "legacy span mode was not migrated to per-display cover mode");
        require(!settings->autoLockEnabled && settings->autoLockTimeoutSeconds == 60,
            "legacy automatic-lock settings were not migrated");
        require(!settings->displayOffAfterLockEnabled,
            "legacy lock-only settings unexpectedly enabled display-off");
        motion::save_settings(path, *settings);
        std::ifstream input(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(json.find("selectedMediaId") != std::string::npos, "canonical media key was not saved");
        require(json.find("selectedVideoId") == std::string::npos, "legacy media key survived migration");
        require(json.find("autoLockEnabled") != std::string::npos &&
            json.find("autoLockTimeoutSeconds") != std::string::npos,
            "canonical automatic-lock settings were not saved");
        require(json.find("displayOffAfterLockEnabled") != std::string::npos &&
            json.find("displayOffAfterLockDelaySeconds") != std::string::npos,
            "canonical post-lock display-off settings were not saved");
        require(json.find("lockTimeoutSeconds") == std::string::npos,
            "legacy lock timeout survived migration");
    }

    void coupled_lock_and_display_off_settings_are_migrated(fs::path const& root)
    {
        auto path = root / L"version-7-settings.json";
        std::ofstream(path) << R"({"version":7,"displayOffEnabled":true,"displayOffTimeoutSeconds":600})";
        auto settings = motion::load_settings(path);
        if (!settings) throw std::runtime_error("version 7 settings could not be loaded");
        require(settings->autoLockEnabled && settings->autoLockTimeoutSeconds == 600,
            "coupled lock timeout was not preserved during migration");
        require(settings->displayOffAfterLockEnabled && settings->displayOffAfterLockDelaySeconds == 30,
            "version 7 settings did not receive the post-lock display-off default");
        motion::save_settings(path, *settings);
        std::ifstream input(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(json.find("displayOffEnabled") == std::string::npos &&
            json.find("displayOffTimeoutSeconds") == std::string::npos,
            "coupled version 7 settings survived migration");
    }

    void unsafe_media_paths_are_rejected(fs::path const& root)
    {
        auto path = root / L"unsafe-media.json";
        std::ofstream(path) << R"({"id":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","groupId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","name":"unsafe","kind":"video","fileName":"../outside.mp4"})";
        require(!motion::load_media(path).has_value(), "media path escaped its library directory");
        require(motion::safe_file_name(L"source.mp4"), "normal media file name was rejected");
        require(!motion::safe_file_name(L"folder/source.mp4"), "nested media file name was accepted");
        require(!motion::safe_file_name(L"C:\\outside.mp4"), "absolute media file name was accepted");
    }

    void desktop_state_is_deterministic()
    {
        motion::Settings settings;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Play, "active desktop should play");
        settings.activePlaybackEnabled = false;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Freeze, "inactive playback should freeze");
        settings.continueWhenCovered = false;
        require(motion::desktop_intent(settings, true, true) == motion::DesktopIntent::Pause, "covered desktop should pause");
        settings.desktopPlayback = false;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Off, "disabled desktop should stop");
        require(motion::desktop_intent(settings, false, false) == motion::DesktopIntent::Off, "missing media should stop");
    }

    void presentation_state_has_explicit_priorities()
    {
        motion::Settings settings;
        settings.idleTimeoutSeconds = 30;
        using motion::agent::RuntimeAction;
        require(motion::agent::reduce_runtime_action(settings, { false, true, false, true, 60 }) == RuntimeAction::DisplayOff,
            "display power-off must win over session lock");
        require(motion::agent::reduce_runtime_action(settings, { true, true, false, true, 60 }) == RuntimeAction::Locked,
            "session lock must win over playback");
        require(motion::agent::reduce_runtime_action(settings, { true, false, true, true, 30 }) == RuntimeAction::ScreensaverPlay,
            "screen saver must win over coverage");
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, true, 0 }) == RuntimeAction::DesktopPlay,
            "mouse or keyboard activity no longer exits screen saver playback");
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, true, 29 }) == RuntimeAction::DesktopPlay,
            "active desktop should play");
        settings.activePlaybackEnabled = false;
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, true, 0 }) == RuntimeAction::DesktopFrozen,
            "inactive desktop should freeze");
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, false, 60 }) == RuntimeAction::Stopped,
            "missing media should stop");
    }

    void tray_controls_preview_and_cycle_without_polling()
    {
        motion::agent::TrayControlState controls;
        require(!controls.ManuallyPaused() && !controls.ScreensaverPreviewActive(),
            "tray controls did not start in the normal playback state");
        controls.TogglePlayback();
        require(controls.ManuallyPaused(), "tray pause command did not pause playback");

        controls.RequestScreensaverPreview(10, 20);
        require(controls.ScreensaverPreviewActive() && controls.ManuallyPaused(),
            "screen saver preview did not preserve the user's paused state");
        require(!controls.ObserveInput(10, 20) && controls.ScreensaverPreviewActive(),
            "screen saver preview dismissed without a new input event");
        require(controls.ObserveInput(10, 21) && !controls.ScreensaverPreviewActive(),
            "a raw input event did not dismiss screen saver preview immediately");
        require(controls.ManuallyPaused(),
            "leaving screen saver preview unexpectedly resumed a paused wallpaper");
        controls.TogglePlayback();
        require(!controls.ManuallyPaused(), "tray resume command did not resume playback");

        std::vector<std::string> ids{ "c", "a", "b", "b" };
        require(motion::agent::next_media_id(ids, "a") == "b",
            "tray next command does not follow a stable media order");
        require(motion::agent::next_media_id(ids, "c") == "a",
            "tray next command does not wrap at the end of a group");
        require(motion::agent::next_media_id(ids, "missing") == "a",
            "tray next command cannot recover from a missing current item");
        require(std::wstring_view(motion::agent::tray_status_text(
            motion::agent::TrayStatus::Screensaver)) == L"屏保运行中",
            "tray current status does not expose screen saver playback");
        require(std::wstring_view(motion::agent::tray_status_text(
                motion::agent::TrayStatus::Failed)) == L"失败" &&
            motion::agent::tray_status_with_renderer_health(
                motion::agent::TrayStatus::Applying, true) ==
                motion::agent::TrayStatus::Failed &&
            motion::agent::tray_status_with_renderer_health(
                motion::agent::TrayStatus::Playing, false) ==
                motion::agent::TrayStatus::Playing,
            "tray status can hide a failed Renderer behind applying or playing");
        require(motion::agent::tray_next_wallpaper_should_advance(true, false) &&
            !motion::agent::tray_next_wallpaper_should_advance(true, true) &&
            !motion::agent::tray_next_wallpaper_should_advance(false, false),
            "a next-wallpaper request can be delayed across a manual pause");

        require(std::wstring_view(motion::agent_command_event_name(
            motion::AgentCommand::TogglePlayback)) == motion::toggle_playback_event_name &&
            std::wstring_view(motion::agent_command_event_name(
                motion::AgentCommand::NextWallpaper)) == motion::next_wallpaper_event_name &&
            std::wstring_view(motion::agent_command_event_name(
                motion::AgentCommand::PreviewScreensaver)) == motion::screensaver_preview_event_name,
            "app and tray commands no longer share the Agent event contract");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        require(static_cast<bool>(agentFile), "Agent source file was not found");
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        require(agent.find("RegisterRawInputDevices") != std::string::npos &&
            agent.find("case WM_INPUT:") != std::string::npos &&
            agent.find("SetScreensaverInputWakeEnabled(screensaverWasActive)") != std::string::npos,
            "screen saver still relies only on the bounded idle poll for wake-up");
        require(agent.find("commandTogglePlayback") != std::string::npos &&
            agent.find("commandNextWallpaper") != std::string::npos &&
            agent.find("commandPreviewScreensaver") != std::string::npos &&
            agent.find("SetTrayStatus(trayStatus") != std::string::npos,
            "tray menu no longer exposes playback, next, screen saver, and current status controls");
    }

    void identifiers_are_path_safe()
    {
        require(motion::valid_id("01234567-89ab-cdef-0123-456789abcdef"), "valid id rejected");
        require(!motion::valid_id("../escape"), "path traversal id accepted");
        require(!motion::valid_id("UPPERCASE"), "uppercase id accepted");
        require(!motion::valid_id(""), "empty id accepted");
    }

    void future_settings_are_rejected(fs::path const& root)
    {
        auto path = root / L"future-settings.json";
        std::ofstream(path) << R"({"version":999,"desktopPlayback":false})";
        require(!motion::load_settings(path).has_value(), "unsupported future settings schema was accepted");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream storeFile(sourceRoot / L"MotionWallpaper.App" / L"SettingsStore.cpp",
            std::ios::binary);
        require(static_cast<bool>(storeFile), "SettingsStore source file was not found");
        std::string store((std::istreambuf_iterator<char>(storeFile)), {});
        require(store.find("SettingsFileStatus::invalid") != std::string::npos &&
            store.find("load_settings(path_).value_or") == std::string::npos,
            "SettingsStore can still overwrite a future schema with defaults");
        std::ifstream windowFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp",
            std::ios::binary);
        require(static_cast<bool>(windowFile), "MainWindow source file was not found");
        std::string window((std::istreambuf_iterator<char>(windowFile)), {});
        require(window.find("settingsWritable = false") != std::string::npos &&
            window.find("if (!settingsWritable)") != std::string::npos,
            "the App can still save defaults after rejecting an existing settings file");
    }

    void custom_library_settings_require_owned_safe_roots(fs::path const& root)
    {
        auto path = root / L"owned-library-settings.json";
        motion::Settings settings;
        auto unowned = fs::absolute(root / L"unowned-custom-library");
        fs::create_directories(unowned / L"Groups");
        settings.mediaLibraryPath = unowned.wstring();
        settings.mediaLibraryId = testLibraryId;
        motion::save_settings(path, settings);
        require(!motion::load_settings(path),
            "an unmarked custom media library was accepted");
        motion::Settings unavailable;
        require(motion::load_settings_file(path, unavailable) ==
            motion::SettingsFileStatus::libraryUnavailable &&
            unavailable.mediaLibraryPath == unowned.lexically_normal().wstring() &&
            unavailable.mediaLibraryId == testLibraryId,
            "an unavailable custom library path/ID was not preserved for UI recovery");

        create_owned_library_root(unowned);
        require(motion::is_owned_media_library(unowned) && motion::load_settings(path),
            "a valid owned custom media library was rejected");
        auto ownershipId = motion::media_library_ownership_id(unowned);
        require(ownershipId && *ownershipId == testLibraryId,
            "the shared media-library ownership marker format was not recognized");

        auto version9Path = root / L"version-9-external-library.json";
        motion::save_settings(version9Path, settings);
        {
            std::ifstream input(version9Path, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(input)), {});
            auto currentVersion = "\"version\":" +
                std::to_string(motion::settings_schema_version);
            auto version = json.find(currentVersion);
            require(version != std::string::npos,
                "current settings did not serialize the expected schema version");
            json.replace(version, currentVersion.size(), "\"version\":9");
            std::ofstream(version9Path, std::ios::binary | std::ios::trunc) << json;
        }
        motion::Settings version9;
        require(motion::load_settings_file(version9Path, version9) ==
            motion::SettingsFileStatus::libraryUnavailable &&
            version9.mediaLibraryPath == unowned.lexically_normal().wstring() &&
            version9.mediaLibraryId.empty(),
            "a v9 external path was silently promoted to a trusted v10 library");

        std::ofstream(unowned / L"user-file.txt") << "not library data";
        require(!motion::is_owned_media_library(unowned) && !motion::load_settings(path),
            "a broad custom directory with unrelated root data was accepted");
        fs::remove(unowned / L"user-file.txt");

        auto reparseTarget = fs::absolute(root / L"reparse-target");
        auto reparseChild = unowned / L"Groups" / L"linked-library-data";
        fs::create_directories(reparseTarget);
        if (CreateSymbolicLinkW(reparseChild.c_str(), reparseTarget.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            require(!motion::is_owned_media_library(unowned) && !motion::load_settings(path),
                "a custom media library containing a reparse point was accepted");
            std::error_code removeError;
            fs::remove(reparseChild, removeError);
        }

        auto deceptiveRoot = fs::absolute(root).root_path() / L"MotionWallpaper-path-check" / L"..";
        settings.mediaLibraryPath = deceptiveRoot.wstring();
        motion::save_settings(path, settings);
        require(!motion::load_settings(path),
            "a path that normalizes to a drive root was accepted");
        motion::Settings rejected;
        require(motion::load_settings_file(path, rejected) == motion::SettingsFileStatus::invalid,
            "a normalized drive-root path was treated as a recoverable media library");
        bool resolverRejected{};
        try { (void)motion::wallpaper_library_directory(root, settings.mediaLibraryPath); }
        catch (...) { resolverRejected = true; }
        require(resolverRejected, "the media-library resolver accepted a normalized drive root");

        auto defaultDataRoot = fs::absolute(root / L"unsafe-default-data");
        auto defaultTarget = fs::absolute(root / L"unsafe-default-target");
        fs::create_directories(defaultDataRoot);
        fs::create_directories(defaultTarget);
        auto defaultLink = defaultDataRoot / L"Wallpapers";
        if (CreateSymbolicLinkW(defaultLink.c_str(), defaultTarget.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            bool unsafeDefaultRejected{};
            try { (void)motion::wallpaper_library_directory(defaultDataRoot); }
            catch (...) { unsafeDefaultRejected = true; }
            require(unsafeDefaultRejected,
                "an existing reparse-point default library was accepted for scanning");
            std::error_code removeError;
            fs::remove(defaultLink, removeError);
        }

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream windowFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp",
            std::ios::binary);
        require(static_cast<bool>(windowFile), "MainWindow source file was not found");
        std::string window((std::istreambuf_iterator<char>(windowFile)), {});
        require(window.find("Load(&mediaLibraryAvailable)") != std::string::npos &&
            window.find("settingsWritable && mediaLibraryAvailable") != std::string::npos &&
            window.find("select_folder(window") < window.find("MoveLibrary(std::move(target))") &&
            window.find("is_owned_media_library(target)") != std::string::npos &&
            window.find("legacy_data_conflict_present(applicationRoot)") != std::string::npos,
            "an unavailable external library can still crash startup, be rebuilt, or block location selection");
    }

    void persisted_library_identity_rejects_same_path_replacement(fs::path const& root)
    {
        auto library = fs::absolute(root / L"persistent-library-identity");
        auto parked = fs::absolute(root / L"persistent-library-identity.original");
        auto settingsPath = root / L"persistent-library-settings.json";
        create_owned_library_root(library, testLibraryId);

        motion::Settings settings;
        settings.mediaLibraryPath = library.wstring();
        settings.mediaLibraryId = testLibraryId;
        motion::save_settings(settingsPath, settings);

        motion::Settings loaded;
        require(motion::load_settings_file(settingsPath, loaded) ==
            motion::SettingsFileStatus::valid &&
            loaded.mediaLibraryPath == library.lexically_normal().wstring() &&
            loaded.mediaLibraryId == testLibraryId,
            "the persisted path/ownership-ID pair did not load");

        fs::rename(library, parked);
        create_owned_library_root(library, replacementLibraryId);
        motion::Settings unavailable;
        require(motion::is_owned_media_library(library) &&
            motion::load_settings_file(settingsPath, unavailable) ==
                motion::SettingsFileStatus::libraryUnavailable &&
            unavailable.mediaLibraryPath == library.lexically_normal().wstring() &&
            unavailable.mediaLibraryId == testLibraryId &&
            !motion::load_settings(settingsPath),
            "a different valid MotionWallpaper library at the same path inherited trust");

        fs::remove_all(library);
        fs::rename(parked, library);
        motion::Settings restored;
        require(motion::load_settings_file(settingsPath, restored) ==
            motion::SettingsFileStatus::valid &&
            restored.mediaLibraryId == testLibraryId,
            "the original library identity did not reconnect after its disk returned");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream windowFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp",
            std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        require(windowFile && agentFile, "App or Agent identity integration source was not found");
        std::string window((std::istreambuf_iterator<char>(windowFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        require(window.find("identity->ownershipId != settings.mediaLibraryId") != std::string::npos &&
            window.find("self->settings.mediaLibraryId = targetTrust->ownershipId") != std::string::npos &&
            agent.find("identity->ownershipId != settings.mediaLibraryId") != std::string::npos,
            "App initialization, library switching, or Agent startup lost the persisted ownership-ID check");
    }

    void media_library_trust_detects_runtime_replacement(fs::path const& root)
    {
        auto library = fs::absolute(root / L"runtime-trust-library");
        auto settingsPath = root / L"runtime-trust-settings.json";
        create_owned_library_root(library);
        motion::Settings settings;
        settings.mediaLibraryPath = library.wstring();
        settings.mediaLibraryId = testLibraryId;
        motion::save_settings(settingsPath, settings);
        auto unchangedSettingsTime = fs::last_write_time(settingsPath);

        auto original = motion::capture_media_library_trust(library);
        require(original && motion::revalidate_media_library_trust(*original),
            "a valid owned library could not acquire a runtime trust identity");
        auto stableGroups = original
            ? motion::media_library_stable_path(*original, library / L"Groups")
            : std::optional<fs::path>{};
        require(original && !original->stableRoot.empty() &&
            original->stableRoot.wstring().starts_with(L"\\\\?\\Volume{") &&
            motion::revalidate_media_library_stable_root(*original) &&
            stableGroups && *stableGroups == original->stableRoot / L"Groups" &&
            !motion::media_library_stable_path(*original, root / L"outside") &&
            motion::same_direct_filesystem_object(library, original->stableRoot),
            "volume-GUID capture or the alias-to-stable mapping is not fail-closed");
        {
            auto lease = motion::acquire_media_library_trust(*original);
            require(static_cast<bool>(lease), "a valid runtime trust lease was rejected");
            std::error_code removeError;
            bool removed = fs::remove(library / motion::media_library_ownership_marker_name, removeError);
            require(!removed && fs::is_regular_file(
                library / motion::media_library_ownership_marker_name),
                "a live trust lease did not pin the ownership marker");
        }

        fs::remove(library / motion::media_library_ownership_marker_name);
        require(!motion::revalidate_media_library_trust(*original),
            "removing the ownership marker did not revoke runtime trust");
        std::ofstream(library / motion::media_library_ownership_marker_name, std::ios::binary)
            << motion::media_library_ownership_marker_prefix
            << testLibraryId << '\n';
        require(!motion::revalidate_media_library_trust(*original),
            "recreating identical marker text bypassed marker FileId validation");

        auto markerReplacement = motion::capture_media_library_trust(library);
        require(static_cast<bool>(markerReplacement),
            "the repaired owned library could not be recaptured");
        auto originalGroups = library / L"Groups.original";
        fs::rename(library / L"Groups", originalGroups);
        fs::create_directory(library / L"Groups");
        require(!motion::revalidate_media_library_trust(*markerReplacement),
            "replacing Groups without changing settings bypassed directory FileId validation");
        fs::remove(library / L"Groups");
        fs::rename(originalGroups, library / L"Groups");

        auto reparseIdentity = motion::capture_media_library_trust(library);
        require(static_cast<bool>(reparseIdentity),
            "the restored Groups identity could not be captured");
        auto reparseTarget = fs::absolute(root / L"runtime-trust-reparse-target");
        fs::create_directories(reparseTarget);
        fs::rename(library / L"Groups", originalGroups);
        auto groupsLink = library / L"Groups";
        if (CreateSymbolicLinkW(groupsLink.c_str(), reparseTarget.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            require(!motion::revalidate_media_library_trust(*reparseIdentity),
                "a runtime Groups reparse replacement retained trust");
            std::error_code linkError;
            fs::remove(groupsLink, linkError);
        }
        if (!fs::exists(groupsLink)) fs::rename(originalGroups, groupsLink);

        auto rootIdentity = motion::capture_media_library_trust(library);
        require(static_cast<bool>(rootIdentity),
            "the restored root identity could not be captured");
        auto originalRoot = fs::absolute(root / L"runtime-trust-library.original");
        fs::rename(library, originalRoot);
        create_owned_library_root(library);
        auto replacementSentinel = library / L"Groups" / L"replacement-sentinel.txt";
        std::ofstream(replacementSentinel, std::ios::binary) << "do not touch";
        require(!motion::revalidate_media_library_trust(*rootIdentity),
            "replacing the library root at the same path bypassed volume/FileId validation");
        bool libraryRejectedReplacement{};
        try {
            motion::app::MediaLibrary guarded(root / L"runtime-trust-data",
                motion::app::DeleteMode::Permanent, library, *rootIdentity);
        } catch (...) {
            libraryRejectedReplacement = true;
        }
        require(libraryRejectedReplacement && fs::is_regular_file(replacementSentinel),
            "MediaLibrary accepted or modified a replacement behind the configured alias");
        require(fs::last_write_time(settingsPath) == unchangedSettingsTime,
            "the runtime replacement regression accidentally relied on a settings timestamp change");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream windowFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp",
            std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        std::ifstream optimizerFile(sourceRoot / L"MotionWallpaper.Agent" / L"VideoOptimizer.cpp",
            std::ios::binary);
        require(windowFile && agentFile && optimizerFile,
            "runtime trust integration source files were not found");
        std::string window((std::istreambuf_iterator<char>(windowFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        std::string optimizer((std::istreambuf_iterator<char>(optimizerFile)), {});
        auto agentGuard = agent.find("loopLibraryTrust");
        auto agentMediaScan = agent.find("imported_optimization_requests(wallpapers)", agentGuard);
        require(window.find("RetainMediaLibraryTrust") != std::string::npos &&
            window.find("acquire_media_library_trust(*mediaLibraryTrust)") != std::string::npos &&
            agentGuard != std::string::npos && agentMediaScan != std::string::npos &&
            agentGuard < agentMediaScan &&
            agent.find("videoOptimizer.reset();", agentGuard) != std::string::npos &&
            optimizer.find("VideoTranscodePathAccess pathAccess") != std::string::npos &&
            optimizer.find("trustLost || !LibraryTrusted()") != std::string::npos,
            "App, Agent, or optimizer can continue path I/O after runtime trust loss");
    }

    void agent_settings_fail_closed_until_recovery(fs::path const& root)
    {
        auto path = root / L"agent-settings-state.json";
        motion::Settings loaded;
        require(motion::load_settings_file(path, loaded) == motion::SettingsFileStatus::missing,
            "a genuinely missing first-run settings file was not distinguished");

        std::ofstream(path) << R"({"version":999,"desktopPlayback":true})";
        loaded.idleTimeoutSeconds = 321;
        require(motion::load_settings_file(path, loaded) == motion::SettingsFileStatus::invalid &&
            loaded.idleTimeoutSeconds == 321,
            "future settings were accepted or destroyed the last parsed destination");

        motion::Settings valid;
        valid.idleTimeoutSeconds = 444;
        motion::save_settings(path, valid);
        require(motion::load_settings_file(path, loaded) == motion::SettingsFileStatus::valid &&
            loaded.idleTimeoutSeconds == 444,
            "the settings state did not recover after a valid document was restored");
        std::ofstream(path, std::ios::trunc) << "{broken";
        require(motion::load_settings_file(path, loaded) == motion::SettingsFileStatus::invalid,
            "a settings file that became corrupt remained enabled");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        require(static_cast<bool>(agentFile), "Agent source file was not found");
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        auto failClosed = agent.find("if (!configurationAvailable || !videoOptimizer)");
        auto scan = agent.find("imported_optimization_requests(wallpapers)", failClosed);
        auto conflictGuard = agent.find("legacy_data_conflict_present(applicationRoot)");
        auto initialLoad = agent.find("load_settings_file(configPath");
        require(initialLoad != std::string::npos && conflictGuard != std::string::npos &&
            conflictGuard < initialLoad &&
            failClosed != std::string::npos && scan != std::string::npos && failClosed < scan &&
            agent.find("same_filesystem_path(configuredWallpapers, wallpapers)") != std::string::npos &&
            agent.find("videoOptimizer.reset();", agent.find("same_filesystem_path(configuredWallpapers, wallpapers)")) != std::string::npos &&
            agent.find("继续使用上一份有效配置") == std::string::npos,
            "the Agent can still scan or optimize a library after settings validation fails");
    }

    void migration_owner_channel_recovers_only_orphaned_requests()
    {
        auto suffix = motion::utf8_to_wide(motion::new_id());
        auto mutexName = L"Local\\MotionWallpaper.Tests.MigrationOwner.Mutex." + suffix;
        auto mappingName = L"Local\\MotionWallpaper.Tests.MigrationOwner.Mapping." + suffix;
        motion::LibraryMigrationOwnerChannel owner(mutexName.c_str(), mappingName.c_str());
        motion::LibraryMigrationOwnerChannel observer(mutexName.c_str(), mappingName.c_str());
        motion::unique_handle requested(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        motion::unique_handle quiesced(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        motion::unique_handle applied(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(owner && observer && requested && quiesced && applied,
            "migration owner test channels could not be created");
        require(owner.TryClaimAndReset(requested.get(), quiesced.get(), applied.get()) &&
            SetEvent(requested.get()) && SetEvent(quiesced.get()),
            "a live migration requester could not claim the protocol");
        require(!observer.TryClaimAndReset(requested.get(), quiesced.get(), applied.get()) &&
            !observer.ClearOrphanedRequest(requested.get(), quiesced.get(), applied.get()) &&
            WaitForSingleObject(requested.get(), 0) == WAIT_OBJECT_0 &&
            WaitForSingleObject(quiesced.get(), 0) == WAIT_OBJECT_0,
            "a second process channel cleared or stole a live migration request");

        // Releasing the exact PID/creation-time/token claim without resetting
        // Requested models the observable state left by a terminated App.
        owner.ReleaseClaim();
        require(observer.ClearOrphanedRequest(requested.get(), quiesced.get(), applied.get()) &&
            WaitForSingleObject(requested.get(), 0) == WAIT_TIMEOUT &&
            WaitForSingleObject(quiesced.get(), 0) == WAIT_TIMEOUT,
            "an orphaned migration request could leave the Agent quiesced forever");
    }

    void media_activity_suspends_idle_time()
    {
        using namespace std::chrono_literals;
        motion::IdleTimer timer;
        require(timer.Update(300s, 10, 120s, false) == 120s, "initial Windows idle time was not preserved");
        require(timer.Update(310s, 10, 130s, true) == 0s, "media activity did not suspend idle time");
        require(timer.Update(370s, 10, 190s, true) == 0s, "idle time advanced during media activity");
        require(timer.Update(371s, 10, 191s, false) == 1s, "idle time did not restart after media activity");
        require(timer.Update(372s, 11, 0s, false) == 0s, "new user input did not reset idle time");
    }

    void audio_allows_screensaver_but_defers_automatic_lock()
    {
        using namespace std::chrono_literals;
        motion::IdleTimer screensaver;
        motion::IdleTimer automaticLock;
        require(screensaver.Update(60s, 10, 30s, false) == 30s, "audio incorrectly deferred the screen saver");
        require(automaticLock.Update(60s, 10, 30s, true) == 0s, "audio did not defer automatic lock");
    }

    void external_media_remains_authoritative_during_own_screensaver()
    {
        using namespace std::chrono_literals;
        require(motion::agent::screensaver_idle_is_inhibited(true, false),
            "external display playback no longer defers the screen saver");
        require(!motion::agent::screensaver_idle_is_inhibited(true, true),
            "the running screen saver reset its own idle timer");
        require(motion::agent::automatic_lock_idle_is_inhibited(true, true, false),
            "external media playback no longer defers automatic lock");
        require(motion::agent::automatic_lock_idle_is_inhibited(true, true, true),
            "external media inhibition was discarded when the screen saver started");
        require(!motion::agent::automatic_lock_idle_is_inhibited(false, false, true),
            "the application's own screen saver inhibited automatic lock without an external media request");
        motion::IdleTimer automaticLock;
        require(automaticLock.Update(30s, 10, 30s, false) == 30s,
            "lock timer did not preserve idle time when the screen saver started");
        require(automaticLock.Update(60s, 10, 60s,
            motion::agent::automatic_lock_idle_is_inhibited(false, false, true)) == 60s,
            "automatic lock did not advance during the application's own screen saver");
        require(automaticLock.Update(61s, 10, 61s,
            motion::agent::automatic_lock_idle_is_inhibited(true, true, true)) == 0s,
            "external playback did not reset automatic-lock idle while the screen saver was active");
    }

    void display_off_waits_for_the_post_lock_delay()
    {
        using namespace std::chrono_literals;
        using motion::agent::display_off_after_lock_is_due;
        require(!display_off_after_lock_is_due(true, 29s, 30, false, true),
            "display powered off before the post-lock delay");
        require(display_off_after_lock_is_due(true, 30s, 30, false, true),
            "display did not power off when the post-lock delay elapsed");
        require(!display_off_after_lock_is_due(false, 30s, 30, false, true),
            "disabled post-lock display-off still fired");
        require(!display_off_after_lock_is_due(true, 30s, 30, true, true),
            "post-lock display-off fired more than once");
        require(!display_off_after_lock_is_due(true, 30s, 30, false, false),
            "post-lock display-off ignored retry backoff");
    }

    void fullscreen_coverage_is_not_limited_to_foreground()
    {
        motion::agent::WindowBounds display{ 0, 0, 1920, 1080 };
        motion::agent::WindowBounds focusedSmall{ 200, 200, 900, 700 };
        motion::agent::WindowBounds backgroundFullscreen{ 0, 0, 1920, 1080 };
        require(!motion::agent::covers_display(focusedSmall, display), "small focused window was treated as fullscreen");
        require(motion::agent::covers_display(backgroundFullscreen, display),
            "non-focused fullscreen window no longer covers the desktop");
    }

    void normal_pause_keeps_decoder_hot()
    {
        require(!motion::renderer::should_compact_idle(false),
            "normal pause still destroys the decoder and causes a resume GPU spike");
        require(motion::renderer::residency_timer_delay_ms(false) == 30'000,
            "idle memory pressure checks became too frequent");
        require(motion::renderer::should_compact_idle(true) &&
            motion::renderer::residency_timer_delay_ms(true) == 250,
            "low-memory pause no longer releases decoder resources promptly");
    }

    void stable_agent_states_do_not_poll_at_twenty_hertz()
    {
        require(motion::agent::runtime_wait_interval_ms(false) == 50,
            "pending Renderer transitions lost their responsive ACK retry");
        require(motion::agent::runtime_wait_interval_ms(true) == 1000,
            "stable playback still wakes the Agent policy loop at high frequency");
        require(motion::agent::runtime_wait_interval_ms(true, true) == 50,
            "short media-library transactions lost their responsive hold interval");
        require(motion::agent::performance_copy_wait_interval_ms(false) == 50,
            "a pending freeze ACK no longer receives a responsive retry");
        require(motion::agent::performance_copy_wait_interval_ms(true) == 500,
            "a long performance-copy task still polls at twenty hertz");
    }

    void battery_power_pauses_optional_variant_generation()
    {
        require(!motion::agent::variant_generation_allowed(true, true, true),
            "battery power still allows background video transcoding");
        require(motion::agent::variant_generation_allowed(false, true, false),
            "an AC-powered priority performance-copy request was blocked");
        require(motion::agent::variant_generation_allowed(false, false, true),
            "AC-powered idle time no longer permits deferred performance-copy work");
        require(!motion::agent::variant_generation_allowed(false, false, false),
            "non-priority transcoding competes with active playback");
    }

    void active_playback_waits_for_selected_performance_copy()
    {
        using motion::agent::active_playback_waits_for_performance_copy;
        require(active_playback_waits_for_performance_copy(true, true),
            "a required balanced, power-saver, or cpu-smooth source fallback kept playing");
        require(!active_playback_waits_for_performance_copy(true, false),
            "a completed balanced copy was blocked from active playback");
        require(!active_playback_waits_for_performance_copy(false, true),
            "original-quality playback was coupled to a derived copy");
        require(motion::agent::performance_copy_preview_required(
                motion::agent::RuntimeAction::DesktopPlay, true) &&
            !motion::agent::performance_copy_preview_required(
                motion::agent::RuntimeAction::DesktopPlay, false) &&
            !motion::agent::performance_copy_preview_required(
                motion::agent::RuntimeAction::ScreensaverPlay, true),
            "static performance previews are not scoped to required desktop routes");

        using motion::agent::optimization_renderer_is_static;
        using motion::agent::RuntimeAction;
        require(!optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, false, false, true, true) &&
            !optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, true, true, true, false) &&
            optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, true, true, true, true),
            "desktop optimization can overlap an unfrozen or retiring source route");
        require(!optimization_renderer_is_static(RuntimeAction::DesktopFrozen,
                false, true, false, true, true) &&
            optimization_renderer_is_static(RuntimeAction::DesktopFrozen,
                false, true, true, true, true) &&
            !optimization_renderer_is_static(RuntimeAction::DesktopPaused,
                false, true, true, true, false) &&
            optimization_renderer_is_static(RuntimeAction::Stopped,
                false, true, false, false, true) &&
            !optimization_renderer_is_static(RuntimeAction::Stopped,
                false, true, false, true, true),
            "frozen, paused, or stopped playback bypassed its Renderer quiescence gate");
        require(motion::agent::source_presentation_may_apply(false, false) &&
            motion::agent::source_presentation_may_apply(true, true) &&
            !motion::agent::source_presentation_may_apply(true, false),
            "a source Renderer can start or resume after optimizer quiescence timed out");

        auto playingAdapter = motion::agent::renderer_preview_adapter_key(L"gpu", false);
        auto previewAdapter = motion::agent::renderer_preview_adapter_key(L"gpu", true);
        auto grouped = motion::agent::group_renderer_routes({
            { L"same-media", L"DISPLAY1", playingAdapter, 1920ULL * 1080 },
            { L"same-media", L"DISPLAY2", previewAdapter, 1920ULL * 1080 }
        }, true);
        require(grouped.size() == 2 && playingAdapter != previewAdapter &&
            grouped[0].monitorDevices.size() == 1 &&
            grouped[1].monitorDevices.size() == 1,
            "a pending display still freezes a same-media sibling Renderer route");
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream optimizerFile(sourceRoot / L"MotionWallpaper.Agent" / L"VideoOptimizer.cpp",
            std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        require(static_cast<bool>(optimizerFile) && static_cast<bool>(agentFile),
            "performance-copy pending sources were not found");
        std::string optimizer((std::istreambuf_iterator<char>(optimizerFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        auto quiesce = optimizer.find("bool Quiesce(uint32_t timeoutMilliseconds)");
        auto worker = optimizer.find("void Run(std::stop_token stop)", quiesce);
        require(quiesce != std::string::npos && worker != std::string::npos,
            "optimizer quiescence implementation disappeared");
        auto quiesceBody = optimizer.substr(quiesce, worker - quiesce);
        auto disable = quiesceBody.find("generationAllowed_ = false");
        auto invalidate = quiesceBody.find("generation_.fetch_add");
        auto clearQueue = quiesceBody.find("pending_.clear()");
        auto waitIdle = quiesceBody.find("condition_.wait_for");
        auto activeIdle = quiesceBody.find("return !active_");
        require(disable != std::string::npos && invalidate != std::string::npos &&
            clearQueue != std::string::npos && waitIdle != std::string::npos &&
            activeIdle != std::string::npos &&
            disable < invalidate && invalidate < clearQueue &&
            clearQueue < waitIdle && waitIdle < activeIdle &&
            optimizer.find("condition_.notify_all()", worker) != std::string::npos,
            "Quiesce can return or admit another job before the active worker becomes idle");
        require(optimizer.find("performanceCopyRequired") != std::string::npos &&
            optimizer.find("EnsureAutomaticRequest") != std::string::npos &&
            agent.find("resolved.performanceCopyRequired") != std::string::npos &&
            agent.find("optimization_renderer_is_static") != std::string::npos &&
            agent.find("resolveVideoOutputs(false, true)") !=
                std::string::npos,
            "Resolve and Agent no longer separate copy necessity from safe generation eligibility");
        require(!motion::agent::runtime_selection_can_publish(true, true) &&
            motion::agent::runtime_selection_can_publish(true, false) &&
            !motion::agent::runtime_selection_can_publish(false, false),
            "a frozen old route can publish new runtime media IDs before replacement first-frame ACK");
    }

    void variant_progress_round_trips_and_resets(fs::path const& root)
    {
        auto mediaDirectory = root / L"variant-progress";
        fs::create_directories(mediaDirectory);

        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "a durable variant request could not be created");
        auto balancedRequest = motion::read_variant_generation_request(mediaDirectory);
        require(balancedRequest.mode == "balanced" && !balancedRequest.requestId.empty(),
            "a new variant request was persisted without an ABA-safe identity");
        auto queued = motion::inspect_variant_cache(mediaDirectory);
        require(queued.queued && !queued.progressKnown && !queued.generating,
            "a newly queued variant exposed fabricated progress");

        require(motion::write_variant_progress_if_current(mediaDirectory, balancedRequest,
            motion::VariantProgressState::generating, 42, true),
            "determinate variant progress could not be persisted");
        auto generating = motion::inspect_variant_cache(mediaDirectory);
        require(generating.generating && generating.progressKnown &&
            generating.progressPercent == 42,
            "variant progress did not round-trip through the cache status");

        require(motion::pause_variant_generation(mediaDirectory),
            "an active variant request could not be paused");
        auto paused = motion::inspect_variant_cache(mediaDirectory);
        require(paused.paused && paused.progressKnown && paused.progressPercent == 42,
            "pausing a variant discarded its last honest percentage");

        require(motion::resume_variant_generation(mediaDirectory),
            "a paused variant request could not be resumed");
        auto resumed = motion::inspect_variant_cache(mediaDirectory);
        require(resumed.queued && !resumed.paused && !resumed.progressKnown,
            "a restarted FFmpeg attempt retained stale percentage data");

        require(motion::write_variant_progress_if_current(mediaDirectory, balancedRequest,
            motion::VariantProgressState::waitingForPower),
            "the battery-waiting state could not be persisted");
        require(motion::inspect_variant_cache(mediaDirectory).waitingForPower,
            "the battery-waiting state was not surfaced to the UI model");

        require(motion::complete_variant_generation(mediaDirectory, balancedRequest),
            "the matching tokenized request could not be completed");
        auto completed = motion::inspect_variant_cache(mediaDirectory);
        require(!completed.queued && !fs::exists(motion::variant_progress_path(mediaDirectory)),
            "completing a variant left stale task progress behind");

        require(motion::request_variant_generation(mediaDirectory, "power-saver"),
            "a second variant request could not be created");
        require(motion::cancel_variant_generation(mediaDirectory),
            "a variant request could not be cancelled");
        auto cancelled = motion::inspect_variant_cache(mediaDirectory);
        require(!cancelled.queued && cancelled.cancelled &&
            !fs::exists(motion::variant_progress_path(mediaDirectory)),
            "cancelling a variant left a live request or stale progress behind");
    }

    void variant_retention_honors_runtime_leases(fs::path const& root)
    {
        auto mediaDirectory = root / L"variant-retention-leases";
        auto variants = mediaDirectory / L"Variants";
        fs::create_directories(variants);
        auto keep = variants / L"balanced-60-1920x1080-v5.mp4";
        auto leased = variants / L"balanced-30-1280x720-v5.mp4";
        std::ofstream(keep, std::ios::binary) << "keep";
        std::ofstream(leased, std::ios::binary) << "leased";

        bool removalWasRechecked{};
        auto retained = motion::retain_variant_profile(mediaDirectory, "balanced",
            keep.filename().wstring(), [&](fs::path const& candidate) {
                removalWasRechecked = candidate == leased;
                return false;
            });
        require(removalWasRechecked && !retained && fs::is_regular_file(leased),
            "profile retention bypassed the runtime lease deletion guard");

        retained = motion::retain_variant_profile(mediaDirectory, "balanced",
            keep.filename().wstring(), [](fs::path const& candidate) {
                std::error_code error;
                return fs::remove(candidate, error) && !error;
            });
        require(retained && !fs::exists(leased) && fs::is_regular_file(keep),
            "profile retention could not finish after the retired lease was released");
    }

    void runtime_variant_cleanup_is_lease_safe_and_atomic()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream optimizerFile(
            sourceRoot / L"MotionWallpaper.Agent" / L"VideoOptimizer.cpp", std::ios::binary);
        require(static_cast<bool>(optimizerFile), "VideoOptimizer source file was not found");
        std::string optimizer((std::istreambuf_iterator<char>(optimizerFile)), {});

        auto prepare = optimizer.find("void Prepare(fs::path const& source");
        auto prepareEnd = optimizer.find("void InvalidateChoices()", prepare);
        auto durableCheck = optimizer.find("if (!durableRequestIsCurrent()) return;", prepare);
        auto sourceProbe = optimizer.find("source_rate(*stableSource)", prepare);
        auto clearFailure = optimizer.find("failed_.erase(key)", prepare);
        require(prepare != std::string::npos && prepareEnd != std::string::npos &&
            durableCheck != std::string::npos && sourceProbe != std::string::npos &&
            clearFailure != std::string::npos &&
            durableCheck < sourceProbe && durableCheck < clearFailure && clearFailure < prepareEnd,
            "Prepare can act on a cached request after its durable marker disappeared");

        auto adopter = optimizer.find("bool TryAdoptVariant(");
        auto adopterEnd = optimizer.find("void Run(std::stop_token", adopter);
        auto adopterLock = optimizer.find("std::lock_guard lock(mutex_)", adopter);
        auto adopterValidation = optimizer.find(
            "current_variant(*stableSource, destinationAccess->path)", adopter);
        require(adopter != std::string::npos && adopterEnd != std::string::npos &&
            adopterLock != std::string::npos && adopterValidation != std::string::npos &&
            adopterLock < adopterValidation && adopterValidation < adopterEnd,
            "variant validation and lease publication are not serialized");
        require(optimizer.find("RemoveVariantIfUnleased(*configured, &protectedFiles)") != std::string::npos &&
            optimizer.find("RemoveVariantIfUnleased(*configured);", adopter) < adopterEnd,
            "cache pruning or profile retention bypasses the per-candidate lease recheck");
        require(optimizer.find("PlaybackVariantIsLeasedLocked(path)") != std::string::npos &&
            optimizer.find("RetainPlaybackVariantLocked(destination,") != std::string::npos,
            "a live Renderer token does not independently protect its exact variant from pruning");
        require(optimizer.find("std::shared_ptr<motion::MediaLibraryTrustLease> libraryTrust") !=
                std::string::npos &&
            optimizer.find("RetainPlaybackVariantLocked(destination, libraryTrust)") !=
                std::string::npos &&
            optimizer.find("RetainLibraryPlaybackLease(trust, acquirePlaybackLease)") !=
                std::string::npos &&
            optimizer.find("RetainLibraryPlaybackLease(trust, true)") != std::string::npos,
            "Renderer playback tokens do not retain external-library identity handles for source and variant paths");
        auto playbackSweep = optimizer.find("void RemoveExpiredPlaybackLeasesLocked()");
        require(playbackSweep != std::string::npos &&
            optimizer.find("lease->second.expired()", playbackSweep) != std::string::npos &&
            optimizer.find("RemoveExpiredPlaybackLeasesLocked();",
                optimizer.find("RetainPlaybackVariantLocked")) != std::string::npos,
            "expired playback registrations can grow without bound across historical media paths");
        require(optimizer.find("choices_") == std::string::npos &&
            optimizer.find("leasedVariants_") == std::string::npos,
            "historical random-playback choices can permanently pin the variant cache");
        auto remover = optimizer.find("bool RemoveVariantIfUnleased(");
        auto finalAlternatePin = optimizer.find(
            "PinAlternatePlayableFileForRemoval(access->path)", remover);
        auto candidateRemoval = optimizer.find("fs::remove(access->path, error)", finalAlternatePin);
        require(remover != std::string::npos && finalAlternatePin != std::string::npos &&
            candidateRemoval != std::string::npos && finalAlternatePin < candidateRemoval,
            "variant pruning can delete without pinning a final alternate playable file");

        auto prune = optimizer.find("void prune_variant_cache(");
        auto pruneEnd = optimizer.find("std::wstring variant_prefix", prune);
        auto identityProbe = optimizer.find(
            "GetFileInformationByHandleEx(file.get(), FileIdInfo", prune);
        // physical_variant_identity is declared immediately before prune, so
        // search that helper independently while keeping the accounting checks
        // bounded to prune's body.
        if (identityProbe == std::string::npos || identityProbe >= pruneEnd) {
            identityProbe = optimizer.find("GetFileInformationByHandleEx(file.get(), FileIdInfo");
        }
        auto volumeIdentity = optimizer.find("identity.VolumeSerialNumber", identityProbe);
        auto fileIdentity = optimizer.find("identity.FileId.Identifier", identityProbe);
        auto aggregate = optimizer.find("allocationByIdentity.try_emplace(", prune);
        auto lastLink = optimizer.find("--allocation.remainingCacheLinks == 0", aggregate);
        auto physicalSubtract = optimizer.find(
            "total = total >= allocation.size ? total - allocation.size : 0", lastLink);
        require(prune != std::string::npos && pruneEnd != std::string::npos &&
            identityProbe != std::string::npos && volumeIdentity != std::string::npos &&
            fileIdentity != std::string::npos && aggregate != std::string::npos &&
            lastLink != std::string::npos && physicalSubtract != std::string::npos &&
            identityProbe < prune && aggregate < lastLink && lastLink < physicalSubtract &&
            physicalSubtract < pruneEnd &&
            optimizer.find("total -= candidate.size", prune) >= pruneEnd,
            "hard-linked variant aliases are double-counted or release cache bytes before the last link is removed");
        require(optimizer.find("storageQuotaBytes_.load", prune) != std::string::npos &&
            optimizer.find("variantCacheLimit") == std::string::npos,
            "optimizer pruning still ignores the configured storage quota or uses a hidden fixed limit");

        std::ifstream agentFile(
            sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        require(static_cast<bool>(agentFile), "Agent source file was not found");
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        require(agent.find("SetStorageQuotaBytes(") != std::string::npos &&
            agent.find("settings.optimizationStorageQuotaBytes") != std::string::npos,
            "the Agent does not apply the user's optimization storage quota");
        auto rendererLease = agent.find("playbackLease_ = media.playbackLease");
        auto processExit = agent.find("WaitForSingleObject(process_.get(), 1200)");
        auto leaseRelease = agent.find("playbackLease_.reset()", processExit);
        require(agent.find("ResolveWithLease(") != std::string::npos &&
            agent.find("AcquirePlaybackLease(") != std::string::npos &&
            rendererLease != std::string::npos && processExit != std::string::npos &&
            leaseRelease != std::string::npos && processExit < leaseRelease,
            "the playback lease is not retained by Renderer until its process has stopped");
        require(agent.find("if (!output.media.playbackLease)") != std::string::npos,
            "static-image paths can reach Renderer without an external-library identity lease");
        require(agent.find("void ReapExited()") != std::string::npos &&
            agent.find("renderer->ReapExited()") != std::string::npos,
            "an exited transition renderer can pin its playback variant indefinitely");


        auto publish = optimizer.find(
            "MoveFileExW(temporary.c_str(), destinationAccess->path.c_str()");
        auto decodeProbe = optimizer.find(
            "bool decodesFirstFrame = video_candidate_decodes_first_frame(temporary)");
        require(publish != std::string::npos &&
            decodeProbe != std::string::npos && decodeProbe < publish &&
            optimizer.find("validateCandidate, &selectedCodec", prepare) < publish &&
            optimizer.find("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", publish) != std::string::npos &&
            optimizer.find("fs::remove(request.destination") == std::string::npos &&
            optimizer.find("fs::rename(temporary, request.destination") == std::string::npos,
            "a completed performance copy is still published through a delete/rename gap");
        auto candidateVisualCheck = optimizer.find(
            "preservesSourceVisualMetadata(actual, codec)", prepare);
        auto finalVisualCheck = optimizer.find(
            "matchingVisualMetadata = preservesSourceVisualMetadata(actual, selectedCodec)",
            candidateVisualCheck);
        require(optimizer.find("MF_MT_TRANSFER_FUNCTION") != std::string::npos &&
            optimizer.find("MF_MT_VIDEO_PRIMARIES") != std::string::npos &&
            optimizer.find("MF_MT_MPEG2_PROFILE") != std::string::npos &&
            candidateVisualCheck != std::string::npos && finalVisualCheck != std::string::npos &&
            candidateVisualCheck < finalVisualCheck && finalVisualCheck < publish,
            "candidate/final acceptance can drop known colour metadata or HEVC Main10 profile");
        auto finalControlBeforePublish = optimizer.rfind("finalControl = control()", publish);
        auto finalControlAfterPublish = optimizer.find("finalControl = control()", publish);
        auto accepted = optimizer.find("accepted = TryAdoptVariant(");
        auto acceptedProgress = optimizer.find(
            "WriteProgress(request.source.parent_path()", accepted);
        require(finalControlBeforePublish != std::string::npos &&
            finalControlAfterPublish != std::string::npos &&
            finalControlBeforePublish < publish && publish < finalControlAfterPublish &&
            accepted != std::string::npos && acceptedProgress != std::string::npos &&
            acceptedProgress < optimizer.find("CompleteGeneration(", accepted),
            "pause/cancel can publish a candidate or report 100% before final adoption");
        auto progressCallback = optimizer.find("auto publishProgress =");
        auto progressAuthority = optimizer.rfind("auto progressIsAuthoritative =", progressCallback);
        auto progressTokenCheck = optimizer.find("durableRequest == request.durableRequest",
            progressAuthority);
        auto progressWrite = optimizer.find(
            "motion::write_variant_progress_if_current(*stableMediaDirectory", progressCallback);
        auto progressPostCheck = optimizer.find("if (!progressIsAuthoritative())", progressWrite);
        auto progressCleanup = optimizer.find("motion::clear_variant_progress(*stableMediaDirectory,",
            progressPostCheck);
        require(progressAuthority != std::string::npos && progressTokenCheck != std::string::npos &&
            progressCallback != std::string::npos && progressWrite != std::string::npos &&
            progressPostCheck != std::string::npos && progressCleanup != std::string::npos &&
            progressAuthority < progressTokenCheck && progressTokenCheck < progressCallback &&
            progressCallback < progressWrite && progressWrite < progressPostCheck &&
            progressPostCheck < progressCleanup &&
            progressCleanup < optimizer.find("};", progressPostCheck),
            "a cancelled or replaced request can resurrect stale generating progress");

        auto stableBoundary = optimizer.find("AcquireStableAccess(");
        auto stableRevalidation = optimizer.find(
            "motion::revalidate_media_library_stable_root(*libraryTrust_)", stableBoundary);
        auto stableMapping = optimizer.find(
            "motion::media_library_stable_path(*libraryTrust_, configuredPath)", stableRevalidation);
        auto normalization = optimizer.find(
            "normalize_variant_profiles(wallpapersAccess->path)");
        auto stablePrune = optimizer.find("prune_variant_cache(*stableWallpapers");
        auto stableTranscode = optimizer.find("*stableSource, temporary", progressCallback);
        require(stableBoundary != std::string::npos && stableRevalidation != std::string::npos &&
            stableMapping != std::string::npos && normalization != std::string::npos &&
            stablePrune != std::string::npos && stableTranscode != std::string::npos &&
            stableBoundary < stableRevalidation && stableRevalidation < stableMapping,
            "optimizer mutations can still follow a reused configured drive letter");
        auto configuredPathReturn = optimizer.find(
            "return { destination, std::move(playbackLease)");
        auto stablePathReturn = optimizer.find("return { destinationAccess->path");
        require(configuredPathReturn != std::string::npos && configuredPathReturn < prepare &&
            stablePathReturn == std::string::npos &&
            optimizer.find("destinationAccess->path.c_str()", publish) != std::string::npos,
            "stable internal publishing leaked a volume-GUID path to Renderer/UI");
        auto transcode = optimizer.find("VideoTranscodeResult Transcode(");
        auto partialToken = optimizer.find("motion::new_variant_request_id()", transcode);
        auto tokenizedPartial = optimizer.find("L\".part-\"", partialToken);
        require(partialToken != std::string::npos && tokenizedPartial != std::string::npos &&
            tokenizedPartial < optimizer.find("L\".part.mp4\"", tokenizedPartial),
            "FFmpeg partial names are not isolated by a high-entropy attempt token");

        std::ifstream cacheFile(
            sourceRoot / L"MotionWallpaper.Common" / L"VariantCache.h", std::ios::binary);
        require(static_cast<bool>(cacheFile), "VariantCache source file was not found");
        std::string cache((std::istreambuf_iterator<char>(cacheFile)), {});
        auto tokenClear = cache.find(
            "VariantGenerationRequest const& expected) noexcept",
            cache.find("inline void clear_variant_progress"));
        auto progressLock = cache.find("GENERIC_READ | DELETE, FILE_SHARE_READ", tokenClear);
        auto progressDisposition = cache.find("mark_locked_request_for_deletion(file.get())", progressLock);
        require(tokenClear != std::string::npos && progressLock != std::string::npos &&
            progressDisposition != std::string::npos && tokenClear < progressLock &&
            progressLock < progressDisposition,
            "token-specific stale-progress cleanup is not a handle-locked compare-and-delete");

        std::ifstream transcoderFile(
            sourceRoot / L"MotionWallpaper.Agent" / L"VideoTranscoder.cpp", std::ios::binary);
        require(static_cast<bool>(transcoderFile), "VideoTranscoder source file was not found");
        std::string transcoder((std::istreambuf_iterator<char>(transcoderFile)), {});
        require(transcoder.find("validateCandidate && !validateCandidate(destination, backend, codec)") !=
                std::string::npos &&
            transcoder.find("fs::remove(destination, fileError)",
                transcoder.find("validateCandidate && !validateCandidate")) != std::string::npos,
            "a backend candidate can bypass decode validation before the next fallback attempt");
        auto stopAndConfirm = transcoder.find("auto stopAndConfirm =");
        auto killChild = transcoder.find("TerminateProcess(processHandle.get()", stopAndConfirm);
        auto killJob = transcoder.find("job.reset();", killChild);
        auto confirmLoop = transcoder.find("for (;;)", killJob);
        auto cancelledStop = transcoder.find("stopAndConfirm(ERROR_CANCELLED)", confirmLoop);
        auto exceptionalStop = transcoder.find("stopAndConfirm(ERROR_PROCESS_ABORTED)", cancelledStop);
        require(stopAndConfirm != std::string::npos && killChild != std::string::npos &&
            killJob != std::string::npos && confirmLoop != std::string::npos &&
            cancelledStop != std::string::npos && exceptionalStop != std::string::npos,
            "FFmpeg cancellation can acknowledge optimizer quiescence before the child exits");
    }

    void first_freeze_keeps_its_compaction_surface()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream rendererFile(
            sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp", std::ios::binary);
        require(static_cast<bool>(rendererFile), "Renderer source file was not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        auto composition = renderer.find("if (compositionChanged)");
        auto preserve = renderer.find("if (!captureForFreeze)", composition);
        auto reset = renderer.find("frozenSurface.Reset()", preserve);
        auto frameComplete = renderer.find("lastTimestamp_ = timestamp", composition);
        require(composition != std::string::npos && preserve < reset && reset < frameComplete,
            "the first Freeze/Pause frame still discards its low-memory compaction surface");

        auto compact = renderer.find("bool compacted = presenter.Compact()");
        auto retry = renderer.find("SetTimer(videoWindow, residencyTimer", compact);
        require(compact != std::string::npos && retry != std::string::npos,
            "a transient frozen-surface compaction failure is never retried");
    }

    void library_migration_is_verified_and_ownership_scoped(fs::path const& root)
    {
        auto createLibrary = [&](fs::path const& source, std::string const& suffix) {
            create_owned_library_root(source);
            auto groupId = "aaaaaaaa-aaaa-aaaa-aaaa-" + suffix;
            auto mediaId = "bbbbbbbb-bbbb-bbbb-bbbb-" + suffix;
            auto mediaDirectory = source / L"Groups" / motion::utf8_to_wide(groupId) /
                L"Videos" / motion::utf8_to_wide(mediaId);
            fs::create_directories(mediaDirectory);
            motion::GroupMetadata group;
            group.id = groupId;
            group.name = L"Migration test";
            group.createdAt = group.updatedAt = motion::timestamp_utc();
            motion::save_group(mediaDirectory.parent_path().parent_path() / L"group.json", group);
            motion::MediaMetadata media;
            media.id = mediaId;
            media.groupId = groupId;
            media.name = media.originalName = L"wallpaper.mp4";
            media.fileName = L"wallpaper.mp4";
            media.kind = "video";
            media.sizeBytes = 24;
            media.revision = 1;
            media.importedAt = media.updatedAt = motion::timestamp_utc();
            motion::save_media(mediaDirectory / L"metadata.json", media);
            std::ofstream(mediaDirectory / media.fileName, std::ios::binary)
                << "verified wallpaper bytes";
            return mediaDirectory / media.fileName;
        };
        auto beginMigration = [&](fs::path const& source, fs::path const& target) {
            auto identity = motion::capture_media_library_trust(source);
            require(identity.has_value(), "migration source identity was not captured");
            return motion::app::LibraryMigrationTransaction::Begin(
                source, target, *identity);
        };
        auto leaveMigrationFromChild = [&](fs::path const& source,
            fs::path const& target, std::wstring_view phase) {
            auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
            auto command = motion::build_command_line({ executable.wstring(),
                L"--leave-library-migration", source.wstring(), target.wstring(),
                std::wstring(phase) });
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION created{};
            require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                &startup, &created) != FALSE,
                "migration crash-fixture process failed to launch");
            motion::unique_handle process(created.hProcess), thread(created.hThread);
            require(WaitForSingleObject(process.get(), 30'000) == WAIT_OBJECT_0,
                "migration crash-fixture process timed out");
            DWORD exitCode{};
            require(GetExitCodeProcess(process.get(), &exitCode) && exitCode == 71,
                "migration crash-fixture process did not stop at the requested phase");
        };
        auto migrationConflicts = [&](fs::path const& target) {
            std::vector<fs::path> result;
            auto prefix = target.filename().wstring() +
                L".MotionWallpaper-migration-conflict-";
            std::error_code error;
            for (fs::directory_iterator entries(target.parent_path(), error), end;
                !error && entries != end; entries.increment(error)) {
                auto name = entries->path().filename().wstring();
                if (entries->is_directory() && name.starts_with(prefix)) {
                    result.push_back(entries->path());
                }
            }
            require(!error, "migration conflict directory enumeration failed");
            return result;
        };

        auto source = root / L"migration-source";
        auto target = root / L"migration-target";
        createLibrary(source, "000000000001");
        {
            auto transaction = beginMigration(source, target);
            transaction->CopyAndVerify();
            transaction->CommitPreparedTarget();
            transaction->MarkActivated();
            auto backup = transaction->ArchiveVerifiedSource();
            require(!fs::exists(source) && fs::is_directory(backup) &&
                fs::is_directory(target / L"Groups"),
                "a verified migration did not activate the target and archive the source");
        }

        auto copyingCrashSource = root / L"migration-copying-crash-source";
        auto copyingCrashTarget = root / L"migration-copying-crash-target";
        auto copyingCrashMedia = createLibrary(copyingCrashSource, "000000000011");
        {
            std::fstream large(copyingCrashMedia,
                std::ios::binary | std::ios::in | std::ios::out);
            large.seekp(8 * 1024 * 1024, std::ios::beg);
            large.put('x');
            require(static_cast<bool>(large),
                "copying crash fixture could not enlarge its source file");
        }
        leaveMigrationFromChild(copyingCrashSource, copyingCrashTarget, L"copying");
        require(fs::is_regular_file(copyingCrashTarget / L".mwm-stage" /
            L".motionwallpaper-migration-state"),
            "copying crash did not retain its durable migration state");
        {
            auto retry = beginMigration(copyingCrashSource, copyingCrashTarget);
            auto conflicts = migrationConflicts(copyingCrashTarget);
            require(conflicts.size() == 1 &&
                fs::is_directory(conflicts.front() / L".mwm-stage") &&
                fs::is_regular_file(conflicts.front() / L".mwm-stage" /
                    L".motionwallpaper-migration-state"),
                "copying crash residue was not atomically preserved as one conflict");
            retry->CopyAndVerify();
            retry->CommitPreparedTarget();
            retry->MarkActivated();
        }
        require(fs::is_directory(copyingCrashSource) &&
            motion::is_owned_media_library(copyingCrashTarget),
            "copying crash retry damaged the source or failed to publish the target");

        auto preparedCrashSource = root / L"migration-prepared-crash-source";
        auto preparedCrashTarget = root / L"migration-prepared-crash-target";
        createLibrary(preparedCrashSource, "000000000012");
        leaveMigrationFromChild(preparedCrashSource, preparedCrashTarget, L"prepared");
        {
            auto retry = beginMigration(preparedCrashSource, preparedCrashTarget);
            auto conflicts = migrationConflicts(preparedCrashTarget);
            require(conflicts.size() == 1 &&
                fs::is_regular_file(conflicts.front() / L".mwm-stage" /
                    L".motionwallpaper-migration-owner"),
                "prepared crash residue was not preserved before retry");
            retry->CopyAndVerify();
            retry->CommitPreparedTarget();
            retry->MarkActivated();
        }

        auto partialCommitSource = root / L"migration-partial-commit-source";
        auto partialCommitTarget = root / L"migration-partial-commit-target";
        createLibrary(partialCommitSource, "000000000013");
        leaveMigrationFromChild(partialCommitSource, partialCommitTarget, L"prepared");
        auto partialStage = partialCommitTarget / L".mwm-stage";
        fs::copy_file(partialStage / L".motionwallpaper-migration-owner",
            partialStage / L"Groups" / L".motionwallpaper-migration-owner");
        fs::copy_file(partialStage / L".motionwallpaper-migration-state",
            partialStage / L"Groups" / L".motionwallpaper-migration-state");
        require(MoveFileExW((partialStage / L"Groups").c_str(),
            (partialCommitTarget / L"Groups").c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "partial-commit crash fixture could not publish Groups");
        {
            auto retry = beginMigration(partialCommitSource, partialCommitTarget);
            auto conflicts = migrationConflicts(partialCommitTarget);
            require(conflicts.size() == 1 &&
                fs::is_directory(conflicts.front() / L"Groups") &&
                fs::is_directory(conflicts.front() / L".mwm-stage"),
                "partial commit was not preserved as a single conflict directory");
            retry->CopyAndVerify();
            retry->CommitPreparedTarget();
            retry->MarkActivated();
        }

        auto tamperedCrashSource = root / L"migration-tampered-crash-source";
        auto tamperedCrashTarget = root / L"migration-tampered-crash-target";
        createLibrary(tamperedCrashSource, "000000000014");
        leaveMigrationFromChild(tamperedCrashSource, tamperedCrashTarget, L"prepared");
        auto tamperedState = tamperedCrashTarget / L".mwm-stage" /
            L".motionwallpaper-migration-state";
        std::ofstream(tamperedState, std::ios::binary | std::ios::app) << "tampered\n";
        bool rejectedTamperedState{};
        try {
            (void)beginMigration(tamperedCrashSource, tamperedCrashTarget);
        } catch (...) {
            rejectedTamperedState = true;
        }
        require(rejectedTamperedState && fs::is_regular_file(tamperedState) &&
            migrationConflicts(tamperedCrashTarget).empty(),
            "an unverifiable migration state was moved, deleted, or reused");

        auto changedSource = root / L"migration-changing-source";
        auto changedTarget = root / L"migration-changing-target";
        createLibrary(changedSource, "000000000002");
        bool detectedChange{};
        try {
            auto transaction = beginMigration(changedSource, changedTarget);
            transaction->CopyAndVerify([&](uint64_t copied, uint64_t total) {
                if (copied == total) {
                    std::ofstream(changedSource / L"concurrent-change.tmp", std::ios::binary)
                        << "changed while copying";
                }
            });
        } catch (...) {
            detectedChange = true;
        }
        require(detectedChange, "a source mutation during migration escaped verification");
        require(fs::is_directory(changedSource) && !fs::exists(changedTarget),
            "a failed migration deleted the source or retained its owned target");

        auto pinnedSource = root / L"migration-pinned-source";
        auto pinnedTarget = root / L"migration-pinned-target";
        auto pinnedReplacement = root / L"migration-pinned-target.replaced";
        createLibrary(pinnedSource, "000000000005");
        auto pinnedTransaction = beginMigration(pinnedSource, pinnedTarget);
        std::error_code pinnedError;
        fs::rename(pinnedTarget, pinnedReplacement, pinnedError);
        if (!pinnedError) {
            fs::create_directories(pinnedTarget);
            std::ofstream(pinnedTarget / L"replacement-sentinel.txt",
                std::ios::binary) << "replacement";
            bool rejectedAliasReplacement{};
            try {
                pinnedTransaction->CopyAndVerify();
            } catch (...) {
                rejectedAliasReplacement = true;
            }
            pinnedTransaction.reset();
            std::ifstream sentinel(pinnedTarget / L"replacement-sentinel.txt",
                std::ios::binary);
            std::string sentinelContents((std::istreambuf_iterator<char>(sentinel)), {});
            require(rejectedAliasReplacement && sentinelContents == "replacement" &&
                fs::is_directory(pinnedReplacement / L".mwm-stage"),
                "migration target alias replacement was written to or cleaned by path");
        } else {
            pinnedTransaction.reset();
            require(!fs::exists(pinnedTarget),
                "rolling back a pinned migration target left owned staging behind");
        }
        require(fs::is_directory(pinnedSource),
            "target identity replacement damaged the migration source");

        auto occupiedTarget = root / L"migration-occupied-target";
        fs::create_directories(occupiedTarget);
        std::ofstream(occupiedTarget / L"user-file.txt") << "keep";
        bool rejectedOccupiedTarget{};
        try {
            (void)beginMigration(changedSource, occupiedTarget);
        } catch (...) {
            rejectedOccupiedTarget = true;
        }
        require(rejectedOccupiedTarget && fs::is_regular_file(occupiedTarget / L"user-file.txt"),
            "migration accepted or damaged a target containing unowned user data");

        auto replacedSource = root / L"migration-replaced-source";
        auto replacedTarget = root / L"migration-replaced-target";
        createLibrary(replacedSource, "000000000004");
        auto replacedIdentity = motion::capture_media_library_trust(replacedSource);
        require(replacedIdentity.has_value(), "replaceable migration identity was not captured");
        fs::remove(replacedSource / motion::media_library_ownership_marker_name);
        std::ofstream(replacedSource / motion::media_library_ownership_marker_name, std::ios::binary)
            << motion::media_library_ownership_marker_prefix
            << "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee\n";
        bool rejectedReplacement{};
        try {
            (void)motion::app::LibraryMigrationTransaction::Begin(
                replacedSource, replacedTarget, *replacedIdentity);
        } catch (...) {
            rejectedReplacement = true;
        }
        require(rejectedReplacement && !fs::exists(replacedTarget),
            "migration accepted a replacement source identity or modified its target");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream migrationFile(
            sourceRoot / L"MotionWallpaper.App" / L"LibraryMigration.cpp",
            std::ios::binary);
        require(static_cast<bool>(migrationFile), "migration source file was not found");
        std::string migrationSource((std::istreambuf_iterator<char>(migrationFile)), {});
        require(migrationSource.find("capture_stable_directory(impl->target)") !=
                std::string::npos &&
            migrationSource.find("impl_->targetAccess") != std::string::npos &&
            migrationSource.find("impl_->RequireTargetTrust()") != std::string::npos,
            "migration target I/O no longer remains bound to its stable volume path");

        auto intrudedSource = root / L"migration-intruded-source";
        auto intrudedTarget = root / L"migration-intruded-target";
        createLibrary(intrudedSource, "000000000003");
        bool cancelledIntrudedCopy{};
        {
            auto transaction = beginMigration(intrudedSource, intrudedTarget);
            std::atomic_bool cancel{};
            try {
                transaction->CopyAndVerify([&](uint64_t, uint64_t) {
                    if (cancel.exchange(true, std::memory_order_acq_rel)) return;
                    std::ofstream(intrudedTarget / L".mwm-stage" / L"user-file.txt",
                        std::ios::binary) << "keep";
                }, &cancel);
            } catch (...) {
                cancelledIntrudedCopy = true;
            }
        }
        std::ifstream preserved(intrudedTarget / L".mwm-stage" / L"user-file.txt",
            std::ios::binary);
        std::string preservedContents((std::istreambuf_iterator<char>(preserved)), {});
        require(cancelledIntrudedCopy && fs::is_directory(intrudedSource) &&
            preservedContents == "keep",
            "migration rollback deleted a file that the transaction did not own");

        motion::app::LibraryAccessGate gate;
        auto writer = gate.TryAcquireWrite();
        require(writer && !gate.TryBeginMigration(),
            "migration entered while an asynchronous library writer was active");
        writer.reset();
        auto migration = gate.TryBeginMigration();
        require(migration && gate.MigrationInProgress() && !gate.TryAcquireWrite(),
            "library writes were not excluded by the migration lease");
        migration.reset();
        require(!gate.MigrationInProgress() && gate.TryAcquireWrite(),
            "the library gate did not reopen after migration");
    }

    void pending_performance_copy_preserves_the_presented_frame()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        require(static_cast<bool>(agentFile), "Agent source file was not found");
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        auto branch = agent.find("else if (waitingForPerformanceCopy)");
        require(branch != std::string::npos, "performance-copy wait branch disappeared");
        auto freezeBarrier = agent.find("renderers.FreezeDisplays(", branch);
        auto previewApply = agent.find(
            "renderers.Apply(Renderer::Target::DesktopPlay", freezeBarrier);
        auto sourceIdleBarrier = agent.rfind(
            "sourcePresentationMayApply(", previewApply);
        auto normalPlaybackBranch = agent.find("switch (state)", previewApply);
        auto retirementBarrier = agent.find("renderers.RetiringRoutesStopped()", previewApply);
        auto generationGate = agent.find(
            "videoOptimizer->SetGenerationAllowed(optimizerMayRun)", retirementBarrier);
        auto currentCopyQueue = agent.find(
            "resolveVideoOutputs(false, true)", generationGate);
        require(freezeBarrier != std::string::npos &&
            previewApply != std::string::npos &&
            sourceIdleBarrier != std::string::npos &&
            normalPlaybackBranch != std::string::npos &&
            retirementBarrier != std::string::npos &&
            generationGate != std::string::npos &&
            currentCopyQueue != std::string::npos &&
            freezeBarrier < sourceIdleBarrier && sourceIdleBarrier < previewApply &&
            previewApply < retirementBarrier &&
            retirementBarrier < generationGate &&
            generationGate < currentCopyQueue,
            "selected-copy generation can start before route freeze, preview ACK, or old-route retirement");
        auto waitBody = agent.substr(branch, normalPlaybackBranch - branch);
        require(agent.find("performanceCopyPreviewOutputs") != std::string::npos &&
            agent.find("renderer_preview_adapter_key") != std::string::npos &&
            waitBody.find("renderers.Apply(Renderer::Target::DesktopFreeze") ==
                std::string::npos,
            "performance preview still globally freezes unrelated displays");
        require(agent.find("media_poster_by_id(") != std::string::npos &&
            agent.find("poster.playbackLease = videoOptimizer->AcquirePlaybackLease(poster.path)") !=
                std::string::npos,
            "the optimization preview no longer prefers a trusted static poster");
        require(agent.find("cloned_or_projected_display_active()") != std::string::npos &&
            agent.find("QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS") != std::string::npos,
            "ordinary extended monitors can still be mistaken for presentation projection");
    }

    void playback_capability_only_degrades_software_devices()
    {
        require(!motion::agent::uses_software_playback("auto", true),
            "automatic playback downgraded a physical video device");
        require(motion::agent::uses_software_playback("auto", true, false),
            "automatic playback ignored a source codec/profile the GPU cannot decode");
        require(motion::agent::uses_software_playback("auto", false) &&
            motion::agent::uses_software_playback("software", true),
            "WARP-only or explicitly software playback missed the CPU profile");
        require(!motion::agent::uses_software_playback("hardware", false, false),
            "strict hardware mode silently changed into software playback");
        require(motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "auto", "software-fallback", "no-d3d11-video-support", true) &&
            motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "auto", "unavailable", "automatic-media-startup", true) &&
            motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "auto", "unavailable", "automatic-first-frame-timeout", true),
            "a real automatic-decoder startup failure did not select cpu-smooth");
        require(!motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "hardware", "unavailable", "automatic-media-startup", true) &&
            !motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "auto", "unavailable", "automatic-media-startup", false) &&
            !motion::agent::automatic_decode_failure_requires_cpu_smooth(
                "auto", "unavailable", "device-lost-after-playback", true),
            "runtime fallback escaped its automatic, adapter-specific startup scope");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream optimizerFile(sourceRoot / L"MotionWallpaper.Agent" / L"VideoOptimizer.cpp",
            std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        require(static_cast<bool>(optimizerFile) && static_cast<bool>(agentFile),
            "codec-aware playback source files were not found");
        std::string optimizer((std::istreambuf_iterator<char>(optimizerFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        require(optimizer.find("GetVideoDecoderConfigCount") != std::string::npos &&
            optimizer.find("D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10") != std::string::npos &&
            optimizer.find("D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2") != std::string::npos &&
            optimizer.find("D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0") != std::string::npos,
            "automatic playback no longer checks source-specific D3D11 decoder configurations");
        require(agent.find("SourceHardwareDecodeAdapter(") != std::string::npos &&
            agent.find("output.decodeAdapter") != std::string::npos &&
            agent.find("routesWithoutHardwareDecode") != std::string::npos &&
            agent.find("AutoDecodeRouteRejected(") != std::string::npos &&
            agent.find("RememberAutoDecodeFailure(") != std::string::npos,
            "the Agent no longer routes unsupported automatic media to cpu-smooth");
        auto resolvedPlayback = agent.find("resolveVideoOutputs();");
        auto resolvedProbe = agent.find(
            "auto unsupportedRoutes = assignDecodeAdapters(outputs)", resolvedPlayback);
        auto cpuResolve = agent.find("resolveVideoOutputs(true);",
            resolvedProbe);
        require(resolvedPlayback != std::string::npos && resolvedProbe != std::string::npos &&
            cpuResolve != std::string::npos && resolvedPlayback < resolvedProbe &&
            resolvedProbe < cpuResolve,
            "automatic playback probes the source instead of the resolved variant, or does not re-resolve cpu-smooth");
        require(agent.find("std::find(route.monitorDevices.begin(), route.monitorDevices.end(),") !=
                std::string::npos &&
            agent.find("value.deviceName) != route.monitorDevices.end()") != std::string::npos,
            "a multi-GPU route can inherit another display adapter's decoder LUID");
        require(agent.find("std::vector<size_t> routesWithoutHardwareDecode") != std::string::npos &&
            agent.find("output.softwarePlaybackTarget = true") != std::string::npos &&
            agent.find("output.playbackFrameRateCap") != std::string::npos &&
            agent.find("routeFrameRateCap") != std::string::npos &&
            agent.find("primaryOnly ? \"primary\" : \"monitor\", routeFrameRateCap") !=
                std::string::npos &&
            agent.find("auto cpuOutputs = display_media_targets") != std::string::npos &&
            agent.find("for (auto index : unsupportedRoutes)") != std::string::npos,
            "one weak decode route still globally degrades media or frame-rate policy");
        std::ifstream rendererFile(sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp",
            std::ios::binary);
        require(static_cast<bool>(rendererFile), "Renderer source file was not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        require(renderer.find("AdapterKey(description) != requiredAdapter") != std::string::npos &&
            renderer.find("requestedDecodeAdapter") != std::string::npos,
            "the probed decoder adapter LUID is not enforced by Renderer");
        require(renderer.find("report_decode_status(\"automatic\", \"first-frame-presented\")") !=
                std::string::npos &&
            renderer.find("report_decode_status(\"unavailable\", \"automatic-media-startup\")") !=
                std::string::npos &&
            agent.find("automatic-first-frame-timeout") != std::string::npos,
            "automatic decode success is still reported before a real frame or startup failures are not fed back");

        auto strong = motion::agent::software_playback_profile(true, 16, 2560, 1600, 165);
        require(strong.enabled && strong.width == 1728 && strong.height == 1080 && strong.frameRate == 60,
            "a strong CPU did not receive the bounded 1080p60 smoothness profile");
        auto medium = motion::agent::software_playback_profile(true, 8, 2560, 1440, 144);
        require(medium.width == 1280 && medium.height == 720 && medium.frameRate == 60,
            "a mid-range CPU did not receive the 720p60 smoothness profile");
        auto modest = motion::agent::software_playback_profile(true, 4, 2560, 1440, 144);
        require(modest.width == 1280 && modest.height == 720 && modest.frameRate == 30,
            "a modest CPU was assigned more than the 720p30 safety budget");
        auto weak = motion::agent::software_playback_profile(true, 2, 2560, 1440, 60);
        require(weak.width == 854 && weak.height == 480 && weak.frameRate == 30,
            "a weak CPU was assigned more than the 480p30 safety budget");
        require(!motion::agent::software_playback_profile(false, 2, 7680, 4320, 240).enabled,
            "physical-GPU playback was unexpectedly constrained by CPU tiering");
    }

    void software_presentation_governor_recovers_without_catchup_bursts()
    {
        require(motion::renderer::software_probe_interval_ms(60) == 17 &&
            motion::renderer::software_probe_interval_ms(30) == 33,
            "software frame pacing no longer has bounded 60/30 FPS waits");
        motion::renderer::SoftwareFrameGovernor governor;
        governor.Configure(60);
        for (int index = 0; index < 7; ++index) {
            require(!governor.Observe(14'000), "software frame cap reacted to a single transient too early");
        }
        require(governor.Observe(14'000) && governor.ActiveFrameRate() == 30,
            "repeated WARP deadline misses did not reduce presentation pressure");
        for (int index = 0; index < 299; ++index) {
            require(!governor.Observe(5'000), "software frame cap recovered before ten stable seconds");
        }
        require(governor.Observe(5'000) && governor.ActiveFrameRate() == 60,
            "software frame cap did not recover after sustained headroom");
    }

    void screensaver_pause_returns_window_to_desktop()
    {
        using motion::renderer::Command;
        using motion::renderer::PresentationMode;
        require(motion::renderer::presentation_hides_cursor(PresentationMode::Screensaver),
            "screen saver presentation no longer hides the pointer");
        require(!motion::renderer::presentation_hides_cursor(PresentationMode::Desktop),
            "desktop wallpaper presentation unexpectedly hides the pointer");
        require(motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::Pause),
            "screen saver pause left its topmost window covering the desktop");
        require(motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::DesktopPlay),
            "screen saver play-to-desktop transition did not restore the desktop host");
        require(!motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::ScreensaverPlay),
            "continuing screen saver playback unexpectedly requested desktop reparenting");
        require(!motion::renderer::leaves_screensaver(PresentationMode::Desktop, Command::Pause),
            "ordinary desktop pause unexpectedly requested reparenting");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream rendererFile(sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp",
            std::ios::binary);
        require(static_cast<bool>(rendererFile), "Renderer source file was not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        auto cursorMessage = renderer.find("case WM_SETCURSOR:");
        auto cursorPolicy = renderer.find("presentation_hides_cursor(presentationMode)", cursorMessage);
        auto cursorRefresh = renderer.find("void refresh_cursor_if_over_renderer()");
        auto firstFrameShown = renderer.find("visualShown = true;");
        auto refreshAfterFirstFrame = renderer.find(
            "refresh_cursor_if_over_renderer()", firstFrameShown);
        auto modeChanged = renderer.find("presentationMode = requested;");
        auto refreshAfterModeChange = renderer.find(
            "refresh_cursor_if_over_renderer()", modeChanged);
        require(cursorMessage != std::string::npos && cursorPolicy != std::string::npos &&
            cursorRefresh != std::string::npos && firstFrameShown != std::string::npos &&
            refreshAfterFirstFrame != std::string::npos && modeChanged != std::string::npos &&
            refreshAfterModeChange != std::string::npos &&
            renderer.find("definition.hCursor = nullptr") != std::string::npos,
            "screen saver cursor hiding is not enforced on movement, first frame, and mode changes");
    }

    void desktop_host_must_cover_the_virtual_screen()
    {
        using motion::renderer::usable_desktop_host_bounds;
        require(usable_desktop_host_bounds(true, 0, 0, 2560, 1440, 0, 0, 2560, 1440),
            "full-size Progman desktop host was rejected");
        require(!usable_desktop_host_bounds(false, 0, 0, 2560, 1440, 0, 0, 2560, 1440),
            "hidden WorkerW desktop host was accepted");
        require(!usable_desktop_host_bounds(true, 0, 0, 136, 39, 0, 0, 2560, 1440),
            "tiny WorkerW desktop host was accepted");
        require(usable_desktop_host_bounds(true, -1920, 0, 2560, 1440, -1920, 0, 2560, 1440),
            "multi-monitor virtual desktop host was rejected");
    }

    void manual_selection_wins_over_group_randomization()
    {
        using motion::agent::RandomSelectionAction;
        using motion::agent::random_selection_action;
        require(random_selection_action(true, false, true, false, false, true) == RandomSelectionAction::UseSelected,
            "manual wallpaper selection did not override the current random item");
        require(random_selection_action(true, false, true, true, true, true) == RandomSelectionAction::UseSelected,
            "explicit user selection lost to a simultaneous automatic trigger");
        require(random_selection_action(true, true, true, false, false, false) == RandomSelectionAction::ChooseRandom,
            "enabling a random group did not choose its initial item");
        require(random_selection_action(false, false, false, true, true, true) == RandomSelectionAction::UseSelected,
            "inactive random group overrode the selected wallpaper");
        require(random_selection_action(true, false, false, false, false, true) == RandomSelectionAction::KeepRandom,
            "stable random selection changed without a trigger");
    }

    void identical_media_share_one_renderer()
    {
        auto shared = motion::agent::renderer_media_key(L"C:\\wallpapers\\valley.mp4", "video");
        auto other = motion::agent::renderer_media_key(L"C:\\wallpapers\\beach.mp4", "video");
        std::vector<motion::agent::RendererRoute> routes{
            { shared, L"\\\\.\\DISPLAY1", L"adapter-a", 1920ull * 1080 },
            { shared, L"\\\\.\\DISPLAY2", L"adapter-a", 2560ull * 1440 }
        };
        auto grouped = motion::agent::group_renderer_routes(routes, true);
        require(grouped.size() == 1 && grouped.front().monitorDevices.size() == 2 &&
            grouped.front().aggregateOutputPixels == 1920ull * 1080 + 2560ull * 1440,
            "the same wallpaper no longer shares one Renderer across displays");

        routes.push_back({ other, L"\\\\.\\DISPLAY3", L"adapter-a", 3840ull * 2160 });
        grouped = motion::agent::group_renderer_routes(routes, true);
        require(grouped.size() == 2, "different wallpapers were incorrectly forced through one Renderer");

        routes.push_back({ shared, L"\\\\.\\DISPLAY4", L"other-adapter", 3840ull * 2160 });
        grouped = motion::agent::group_renderer_routes(routes, true);
        auto alternateAdapter = std::find_if(grouped.begin(), grouped.end(), [](auto const& route) {
            return route.adapterKey == L"other-adapter";
        });
        require(grouped.size() == 3 && alternateAdapter != grouped.end() &&
            alternateAdapter->aggregateOutputPixels == 3840ull * 2160,
            "the same wallpaper was incorrectly shared across display adapters");

        auto saturated = motion::agent::group_renderer_routes({
            { shared, L"\\\\.\\DISPLAY1", L"adapter-a", (std::numeric_limits<uint64_t>::max)() },
            { shared, L"\\\\.\\DISPLAY2", L"adapter-a", 1 }
        }, true);
        require(saturated.size() == 1 &&
            saturated.front().aggregateOutputPixels == (std::numeric_limits<uint64_t>::max)(),
            "aggregate display load overflowed instead of saturating");

        auto firstUnknown = motion::agent::renderer_adapter_key({}, L"\\\\.\\DISPLAY5");
        auto secondUnknown = motion::agent::renderer_adapter_key({}, L"\\\\.\\DISPLAY6");
        require(!firstUnknown.empty() && firstUnknown != secondUnknown,
            "unknown indirect-display adapters collapse into one Renderer route");

        grouped = motion::agent::group_renderer_routes({
            { shared, L"\\\\.\\DISPLAY1", {}, 1920ull * 1080 },
            { shared, L"\\\\.\\DISPLAY2", {}, 1280ull * 720 }
        }, false);
        require(grouped.size() == 1 && grouped.front().monitorDevices.empty() &&
            grouped.front().aggregateOutputPixels == 1920ull * 1080 + 1280ull * 720,
            "primary-only routing lost output load or unexpectedly retained monitor routes");
    }

    void video_variant_policy_preserves_quality_priority()
    {
        using motion::agent::VideoSourceCodec;
        using motion::agent::VideoHardwareDecodeProfile;
        require(motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, true, 1),
            "8-bit HEVC Main unexpectedly lost the bounded software fallback");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, true, 2),
            "HEVC Main10 was allowed to fall through to an 8-bit software encoder");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, false, 0),
            "unknown HEVC bit depth was treated as safe for 8-bit software encoding");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::H264, true, 100, true),
            "HDR transfer metadata was ignored by the software fallback guard");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Unknown, false, 0) &&
            !motion::agent::video_software_fallback_allowed(VideoSourceCodec::Vp9, true, 2) &&
            !motion::agent::video_software_fallback_allowed(VideoSourceCodec::Av1, true, 0),
            "unknown or potentially high-bit-depth codecs were flattened into H.264");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Unknown, false, 0, false, true),
            "BT.2020 primaries were ignored by the software fallback guard");
        require(motion::agent::video_hardware_decode_profile(
                VideoSourceCodec::H264, true, 100) == VideoHardwareDecodeProfile::H264 &&
            motion::agent::video_hardware_decode_profile(
                VideoSourceCodec::Hevc, true, 2) == VideoHardwareDecodeProfile::HevcMain10 &&
            motion::agent::video_hardware_decode_profile(
                VideoSourceCodec::Vp9, true, 2) == VideoHardwareDecodeProfile::Vp9Profile2 &&
            motion::agent::video_hardware_decode_profile(
                VideoSourceCodec::Av1, true, 0) == VideoHardwareDecodeProfile::Av1Profile0 &&
            motion::agent::video_hardware_decode_profile(
                VideoSourceCodec::Av1, false, 0) == VideoHardwareDecodeProfile::Unsupported,
            "source codec/profile metadata no longer maps conservatively to D3D11 decoder profiles");
        require(motion::agent::video_cpu_conversion_allowed(false, false) &&
            !motion::agent::video_cpu_conversion_allowed(true, false) &&
            !motion::agent::video_cpu_conversion_allowed(false, true),
            "CPU compatibility copies no longer distinguish SDR Main10 from HDR/BT.2020");
        require(motion::agent::video_variant_color_metadata_matches(
                true, 16, true, 16, true, 9, true, 9) &&
            !motion::agent::video_variant_color_metadata_matches(
                true, 16, false, 0, true, 9, true, 9) &&
            !motion::agent::video_variant_color_metadata_matches(
                true, 16, true, 1, true, 9, true, 9) &&
            !motion::agent::video_variant_color_metadata_matches(
                true, 16, true, 16, true, 9, true, 1) &&
            motion::agent::video_variant_color_metadata_matches(
                false, 0, false, 0, false, 0, false, 0),
            "a performance copy can lose or change known transfer/primaries metadata");
        require(motion::agent::video_variant_is_hevc_main10(true, 2) &&
            !motion::agent::video_variant_is_hevc_main10(true, 1) &&
            !motion::agent::video_variant_is_hevc_main10(false, 2),
            "HEVC performance-copy validation does not require a known Main10 profile");
        auto original = motion::agent::video_variant_decision("original");
        require(!original.targetFps && original.fileName.empty(), "original mode unexpectedly requested a proxy");
        auto balanced = motion::agent::video_variant_decision("balanced", 2560, 1440, 240, 1, 165);
        require(balanced.targetFps == 120 && balanced.fileName == L"balanced-120-2560x1440-v5.mp4",
            "balanced mode no longer targets the high-quality 120 FPS proxy");
        auto sixtyHertz = motion::agent::video_variant_decision("balanced", 2560, 1440, 240, 1, 60);
        require(sixtyHertz.targetFps == 60 && sixtyHertz.fileName == L"balanced-60-2560x1440-v5.mp4",
            "balanced mode generated frames the display cannot present");
        auto powerSaver = motion::agent::video_variant_decision("power-saver", 2560, 1440, 240, 1, 165);
        require(powerSaver.targetFps == 60 && powerSaver.fileName == L"power-saver-60-2560x1440-v5.mp4",
            "power saver did not retain its explicit 60 FPS policy");
        auto cpuSmooth = motion::agent::video_variant_decision("cpu-smooth", 1280, 720, 240, 1, 60);
        require(cpuSmooth.targetFps == 60 && cpuSmooth.fileName == L"cpu-smooth-60-1280x720-v5.mp4",
            "software playback did not receive its isolated CPU-friendly cache identity");
        auto nativeRate = motion::agent::video_variant_decision("balanced", 2560, 1440, 30, 1, 165);
        require(nativeRate.targetFps == 30, "balanced mode inserted frames missing from the source");
        require(motion::agent::video_variant_rate_matches(60'000, 1'001, 60),
            "59.94 FPS container rate was incorrectly rejected as non-60 FPS");
        require(!motion::agent::video_variant_rate_matches(120, 1, 60),
            "a different performance tier passed the variant frame-rate check");
        require(motion::agent::video_variant_dimensions_match(3'840, 2'176, 3'840, 2'160),
            "valid HEVC coding-block padding was rejected");
        require(!motion::agent::video_variant_dimensions_match(1'920, 1'080, 3'840, 2'160),
            "a different visible resolution passed variant validation");
        constexpr uint64_t second = 10'000'000;
        require(motion::agent::video_variant_duration_matches(598 * second, 600 * second, 60),
            "normal CFR/container duration drift was rejected");
        require(!motion::agent::video_variant_duration_matches(590 * second, 600 * second, 60),
            "a materially truncated variant passed duration validation");
        require(!motion::agent::video_variant_duration_matches(0, 600 * second, 60) &&
            motion::agent::video_variant_duration_matches(2 * second, 0, 60),
            "unknown and empty duration handling is not fail-safe");
        require(motion::agent::video_needs_variant(240, 1, 120), "240 FPS source was not optimized");
        require(!motion::agent::video_needs_variant(120, 1, 120), "120 FPS source was unnecessarily transcoded");
        require(motion::agent::video_needs_variant(60, 1, 60, 3840, 2160, 2560, 1440),
            "display-resolution optimization was skipped when frame rate already matched");
        auto fitted = motion::agent::video_variant_dimensions(3'840, 2'160, 2'560, 1'600);
        require(fitted.first == 2'846 && fitted.second == 1'600,
            "display-aware variant no longer preserves cover resolution");
        auto native = motion::agent::video_variant_dimensions(3'840, 2'160, 3'840, 2'160);
        require(native.first == 3'840 && native.second == 2'160,
            "display-aware variant unexpectedly upscaled or cropped a native 4K target");
        auto cpuWide = motion::agent::video_cpu_variant_dimensions(3'840, 720, 1'920, 1'080);
        require(cpuWide.first == 1'920 && cpuWide.second == 360,
            "ultrawide CPU playback escaped the hard software pixel budget");
        auto cpuPortrait = motion::agent::video_cpu_variant_dimensions(2'160, 3'840, 1'080, 1'920);
        require(cpuPortrait.first == 1'080 && cpuPortrait.second == 1'920,
            "portrait CPU playback no longer preserves source aspect ratio");
    }

    void variant_requests_use_last_writer_wins(fs::path const& root)
    {
        auto mediaDirectory = root / L"variant-request-order";
        fs::create_directories(mediaDirectory / L"Variants");

        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "balanced request could not be persisted");
        auto obsoleteBalanced = motion::read_variant_generation_request(mediaDirectory);
        require(motion::request_variant_generation(mediaDirectory, "power-saver"),
            "newer power-saver request could not replace balanced");
        require(motion::pause_variant_generation(mediaDirectory) &&
            motion::inspect_variant_cache(mediaDirectory).paused,
            "a queued optimization request could not be paused");
        require(motion::resume_variant_generation(mediaDirectory) &&
            !motion::inspect_variant_cache(mediaDirectory).paused,
            "a paused optimization request could not be resumed");
        require(!motion::complete_variant_generation(mediaDirectory, obsoleteBalanced),
            "an obsolete token unexpectedly completed a newer request");
        require(motion::read_variant_request(mediaDirectory) == "power-saver",
            "an obsolete completion cleared the newer request");
        require(!motion::fail_variant_generation(mediaDirectory, obsoleteBalanced),
            "an obsolete token unexpectedly failed a newer request");
        require(motion::read_variant_request(mediaDirectory) == "power-saver" &&
            !fs::exists(motion::variant_failed_path(mediaDirectory)),
            "an obsolete failure replaced the newer request");

        auto legacyBalanced = mediaDirectory / L"Variants" / L"balanced-120-2560x1440-v4.mp4";
        auto currentBalanced = mediaDirectory / L"Variants" / L"balanced-120-2560x1440-v5.mp4";
        std::ofstream(legacyBalanced, std::ios::binary) << "legacy-balanced";
        std::ofstream(currentBalanced, std::ios::binary) << "balanced";
        std::ofstream(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4",
            std::ios::binary) << "legacy";
        auto status = motion::inspect_variant_cache(mediaDirectory);
        require(status.requestedMode == "power-saver" && status.entries.size() == 3,
            "variant status lost the active request or cached files");
        require(motion::select_variant_file(status, "balanced") == currentBalanced.filename().wstring() &&
            motion::select_variant_file(status, "original") == currentBalanced.filename().wstring() &&
            motion::select_variant_file(status, "power-saver").starts_with(L"power-saver-"),
            "retained variant selection no longer prefers v5 within the requested profile");
        auto balanced = std::find_if(status.entries.begin(), status.entries.end(), [](auto const& entry) {
            return entry.mode == "balanced";
        });
        auto powerSaver = std::find_if(status.entries.begin(), status.entries.end(), [](auto const& entry) {
            return entry.mode == "power-saver";
        });
        require(balanced != status.entries.end() && powerSaver != status.entries.end(),
            "performance cache files were assigned to the wrong profile");

        auto currentPowerSaver = mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v5.mp4";
        std::ofstream(currentPowerSaver, std::ios::binary) << "current";
        require(!motion::retain_variant_profile(mediaDirectory, "power-saver", L"missing.mp4") &&
            fs::is_regular_file(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4"),
            "profile retention removed a cache before validating its keep target");
        require(motion::retain_variant_profile(mediaDirectory, "power-saver",
            currentPowerSaver.filename().wstring()) &&
            !fs::exists(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4") &&
            fs::is_regular_file(currentPowerSaver) &&
            fs::is_regular_file(legacyBalanced) && fs::is_regular_file(currentBalanced),
            "profile retention did not remove only the superseded same-tier copy");

        auto sharedDirectory = root / L"variant-shared-storage";
        fs::create_directories(sharedDirectory / L"Variants");
        auto sharedBalanced = sharedDirectory / L"Variants" / L"balanced-60-2560x1440-v5.mp4";
        auto sharedPowerSaver = sharedDirectory / L"Variants" / L"power-saver-60-2560x1440-v5.mp4";
        std::ofstream(sharedBalanced, std::ios::binary) << "shared-copy";
        fs::create_hard_link(sharedBalanced, sharedPowerSaver);
        auto sharedStatus = motion::inspect_variant_cache(sharedDirectory);
        require(sharedStatus.entries.size() == 2 && sharedStatus.files == 1 &&
            sharedStatus.bytes == fs::file_size(sharedBalanced) &&
            sharedStatus.entries[0].sharedStorage && sharedStatus.entries[1].sharedStorage,
            "hard-linked performance profiles were counted as duplicate physical storage");

        auto powerSaverRequest = motion::read_variant_generation_request(mediaDirectory);
        require(motion::fail_variant_generation(mediaDirectory, powerSaverRequest),
            "the current tokenized request could not be failed");
        auto failedStatus = motion::inspect_variant_cache(mediaDirectory);
        require(failedStatus.failed && failedStatus.failedMode == "power-saver" &&
            failedStatus.requestedMode.empty(),
            "a failed performance-copy task did not retain its retryable profile identity");
        require(motion::request_variant_generation(mediaDirectory, "power-saver"),
            "a failed performance-copy task could not be retried");
        auto retriedPowerSaver = motion::read_variant_generation_request(mediaDirectory);
        require(motion::complete_variant_generation(mediaDirectory, retriedPowerSaver),
            "the retried tokenized request could not be completed");
        require(motion::read_variant_request(mediaDirectory).empty(),
            "the matching completion did not clear its request");

        require(motion::suppress_variant_generation(mediaDirectory, "power-saver"),
            "a deleted profile could not persist its suppression marker");
        require(motion::variant_generation_suppressed(mediaDirectory, "power-saver"),
            "a deleted profile would be regenerated automatically");
        require(motion::request_variant_generation(mediaDirectory, "power-saver") &&
            !motion::variant_generation_suppressed(mediaDirectory, "power-saver"),
            "manual generation did not re-enable a deleted profile");
    }

    void same_mode_variant_retry_rejects_stale_worker(fs::path const& root)
    {
        auto mediaDirectory = root / L"variant-same-mode-retry";
        fs::create_directories(mediaDirectory);

        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "the first balanced request could not be persisted");
        auto first = motion::read_variant_generation_request(mediaDirectory);
        require(first.mode == "balanced" && !first.requestId.empty(),
            "the first balanced request has no identity");
        require(motion::cancel_variant_generation(mediaDirectory),
            "the first balanced request could not be cancelled");
        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "the same-mode retry could not be persisted");
        auto retry = motion::read_variant_generation_request(mediaDirectory);
        require(retry.mode == "balanced" && !retry.requestId.empty() &&
            retry.requestId != first.requestId,
            "a same-mode retry reused the cancelled request identity");
        require(motion::write_variant_progress_if_current(mediaDirectory, retry,
                motion::VariantProgressState::generating, 7, true) &&
            !motion::write_variant_progress_if_current(mediaDirectory, first,
                motion::VariantProgressState::generating, 100, true),
            "a stale worker could overwrite the retry's progress");
        motion::clear_variant_progress(mediaDirectory, first);
        require(!motion::complete_variant_generation(mediaDirectory, first) &&
            !motion::fail_variant_generation(mediaDirectory, first),
            "a stale same-mode worker could mutate the retry terminal state");
        auto status = motion::inspect_variant_cache(mediaDirectory);
        require(status.requestedMode == "balanced" && status.queued && status.generating &&
            status.progressKnown && status.progressPercent == 7 && !status.failed,
            "a stale same-mode worker cleared or replaced the retry state");

        auto legacyDirectory = root / L"variant-legacy-request";
        fs::create_directories(legacyDirectory);
        require(motion::write_small_file(motion::variant_request_path(legacyDirectory), "balanced"),
            "a legacy request marker could not be created");
        auto legacy = motion::read_variant_generation_request(legacyDirectory);
        require(legacy.mode == "balanced" && legacy.requestId.empty() &&
            motion::complete_variant_generation(legacyDirectory, legacy),
            "a pre-token request marker is no longer compatible");
    }

    void video_transcoder_fails_closed_without_backend(fs::path const& root)
    {
        std::wstring error;
        auto result = motion::agent::transcode_video(
            root / L"missing-ffmpeg.exe", root / L"source.mp4", root / L"output.mp4",
            3840, 2160, 60, [] { return motion::agent::VideoTranscodeControl::running; }, error);
        require(result == motion::agent::VideoTranscodeResult::unsupported && !error.empty(),
            "missing optimization backend did not safely fall back to the source video");
    }

    void media_foundation_candidate_probe_decodes_a_real_first_frame(fs::path const& root)
    {
        require(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)),
            "Media Foundation could not start for the decode-probe integration test");
        struct MediaFoundationShutdown
        {
            ~MediaFoundationShutdown() { MFShutdown(); }
        } shutdown;

        constexpr UINT32 width = 64;
        constexpr UINT32 height = 64;
        constexpr UINT32 fps = 30;
        auto samplePath = root / L"decode-probe-h264.mp4";
        Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
        require(SUCCEEDED(MFCreateSinkWriterFromURL(
            samplePath.c_str(), nullptr, nullptr, &writer)),
            "the H.264 integration sample writer could not be created");

        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        require(SUCCEEDED(MFCreateMediaType(&outputType)) &&
            SUCCEEDED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
            SUCCEEDED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) &&
            SUCCEEDED(outputType->SetUINT32(MF_MT_AVG_BITRATE, 250'000)) &&
            SUCCEEDED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
            SUCCEEDED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height)) &&
            SUCCEEDED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1)) &&
            SUCCEEDED(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)),
            "the H.264 integration output type could not be configured");
        DWORD stream{};
        require(SUCCEEDED(writer->AddStream(outputType.Get(), &stream)),
            "the H.264 integration output stream could not be added");

        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        require(SUCCEEDED(MFCreateMediaType(&inputType)) &&
            SUCCEEDED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
            SUCCEEDED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) &&
            SUCCEEDED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
            SUCCEEDED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height)) &&
            SUCCEEDED(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1)) &&
            SUCCEEDED(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) &&
            SUCCEEDED(writer->SetInputMediaType(stream, inputType.Get(), nullptr)) &&
            SUCCEEDED(writer->BeginWriting()),
            "the H.264 integration input type could not be configured");

        constexpr DWORD frameBytes = width * height * 3 / 2;
        constexpr LONGLONG frameDuration = 10'000'000 / fps;
        for (LONGLONG frame = 0; frame < fps; ++frame) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            require(SUCCEEDED(MFCreateMemoryBuffer(frameBytes, &buffer)),
                "the H.264 integration frame buffer could not be allocated");
            BYTE* bytes{};
            DWORD capacity{};
            require(SUCCEEDED(buffer->Lock(&bytes, &capacity, nullptr)) && capacity >= frameBytes,
                "the H.264 integration frame buffer could not be locked");
            std::fill_n(bytes, width * height, static_cast<BYTE>(16 + frame % 180));
            std::fill_n(bytes + width * height, width * height / 2, static_cast<BYTE>(128));
            buffer->Unlock();
            require(SUCCEEDED(buffer->SetCurrentLength(frameBytes)),
                "the H.264 integration frame length could not be set");

            Microsoft::WRL::ComPtr<IMFSample> sample;
            require(SUCCEEDED(MFCreateSample(&sample)) &&
                SUCCEEDED(sample->AddBuffer(buffer.Get())) &&
                SUCCEEDED(sample->SetSampleTime(frame * frameDuration)) &&
                SUCCEEDED(sample->SetSampleDuration(frameDuration)) &&
                SUCCEEDED(writer->WriteSample(stream, sample.Get())),
                "the H.264 integration frame could not be encoded");
        }
        require(SUCCEEDED(writer->Finalize()) && fs::is_regular_file(samplePath),
            "the H.264 integration sample could not be finalized");
        writer.Reset();
        require(motion::agent::video_candidate_decodes_first_frame(samplePath),
            "a valid local H.264 sample failed the real first-frame decode probe");

        auto invalidPath = root / L"decode-probe-invalid.mp4";
        std::ofstream(invalidPath, std::ios::binary) << "not a compressed video";
        require(!motion::agent::video_candidate_decodes_first_frame(invalidPath),
            "an invalid compressed sample passed the first-frame decode probe");

        // Exercise the real optimizer with a decodable source. Generation is
        // globally disabled so this test observes the exact policy boundary:
        // the selected copy remains required/static, while its durable UI
        // request is visible and stable without a source+transcode overlap.
        // Keep this policy fixture below the legacy Win32 path boundary. The
        // marker writer adds a per-process/thread suffix, and long-path I/O is
        // unrelated to the optimizer state transitions exercised here.
        auto optimizerRoot = root / L"optimizer-policy";
        auto mediaDirectory = optimizerRoot / L"Groups" /
            L"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa" / L"Videos" /
            L"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        fs::create_directories(mediaDirectory);
        auto optimizerSource = mediaDirectory / L"source.mp4";
        require(fs::copy_file(samplePath, optimizerSource,
                fs::copy_options::overwrite_existing),
            "the optimizer integration source could not be staged");
        motion::agent::VideoOptimizer optimizer(optimizerRoot,
            root / L"optimizer-policy-log", root / L"missing-application");
        optimizer.SetGenerationAllowed(false);

        auto balanced = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        auto automaticRequest =
            motion::read_variant_generation_request(mediaDirectory);
        auto automaticStatus = motion::inspect_variant_cache(mediaDirectory);
        require(balanced.path == optimizerSource &&
            balanced.performanceCopyRequired &&
            !balanced.performanceCopyPending &&
            automaticRequest.mode == "balanced" &&
            !automaticRequest.requestId.empty() &&
            automaticStatus.queued && !automaticStatus.generating,
            "a blocked selected copy played its source or failed to publish durable progress");
        auto repeated = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        require(repeated.performanceCopyRequired &&
            motion::read_variant_generation_request(mediaDirectory) == automaticRequest,
            "Resolve recreated the automatic request on every policy pass");
        auto original = optimizer.ResolveWithLease(
            optimizerSource, "original", 32, 32, 30, false, true);
        auto cpuSmooth = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, true, true);
        require(!original.performanceCopyRequired &&
            cpuSmooth.performanceCopyRequired &&
            !cpuSmooth.performanceCopyPending,
            "original or cpu-smooth playback lost its distinct copy requirement");

        require(motion::pause_variant_generation(mediaDirectory),
            "the optimizer's automatic request could not be paused");
        auto paused = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        require(paused.performanceCopyRequired &&
            !paused.performanceCopyPending &&
            motion::read_variant_generation_request(mediaDirectory) == automaticRequest &&
            motion::variant_generation_paused(mediaDirectory),
            "Resolve ignored or replaced a paused automatic request");
        require(motion::resume_variant_generation(mediaDirectory) &&
            motion::cancel_variant_generation(mediaDirectory),
            "the automatic request could not be resumed and cancelled");
        auto cancelled = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        require(cancelled.performanceCopyRequired &&
            !cancelled.performanceCopyPending &&
            !motion::read_variant_generation_request(mediaDirectory) &&
            fs::is_regular_file(motion::variant_cancelled_path(mediaDirectory)),
            "Resolve recreated a user-cancelled automatic request");

        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "the suppression-state integration request could not be created");
        require(motion::suppress_variant_generation(mediaDirectory, "balanced"),
            "the integration request could not be suppressed");
        auto suppressed = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        require(suppressed.performanceCopyRequired &&
            !motion::read_variant_generation_request(mediaDirectory) &&
            motion::variant_generation_suppressed(mediaDirectory, "balanced"),
            "Resolve recreated a suppressed automatic request");
        motion::unsuppress_variant_generation(mediaDirectory, "balanced");
        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "the failure-state integration request could not be created");
        auto failedRequest =
            motion::read_variant_generation_request(mediaDirectory);
        require(motion::fail_variant_generation(mediaDirectory, failedRequest),
            "the failure-state integration request could not be failed");
        auto failed = optimizer.ResolveWithLease(
            optimizerSource, "balanced", 32, 32, 30, false, true);
        require(failed.performanceCopyRequired &&
            !failed.performanceCopyPending &&
            !motion::read_variant_generation_request(mediaDirectory) &&
            fs::is_regular_file(motion::variant_failed_path(mediaDirectory)),
            "Resolve recreated a failed automatic request");
        optimizer.SetGenerationAllowed(true);
        require(optimizer.Quiesce(100),
            "an idle optimizer could not acknowledge the source-playback barrier");
    }

    void video_transcoder_orders_vendor_backends_and_bounds_software_fallback()
    {
        using motion::agent::VideoTranscodeAdapter;
        using motion::agent::VideoTranscodeBackend;
        using motion::agent::VideoTranscodeCodec;
        require(motion::agent::video_transcode_backend_codec(
            VideoTranscodeBackend::nvidiaNvenc, true) == VideoTranscodeCodec::H264 &&
            motion::agent::video_transcode_backend_codec(
                VideoTranscodeBackend::nvidiaNvenc, false) == VideoTranscodeCodec::HevcMain10 &&
            motion::agent::video_transcode_backend_codec(
                VideoTranscodeBackend::softwareOpenH264, false) == VideoTranscodeCodec::H264,
            "safe SDR and protected HDR work no longer select distinct output codecs");

        auto unboundedHevc = motion::agent::video_transcode_rate_control(
            2560, 1440, 60, VideoTranscodeCodec::HevcMain10);
        auto boundedHevc = motion::agent::video_transcode_rate_control(
            2560, 1440, 60, VideoTranscodeCodec::HevcMain10,
            130'000'000, 843'600'000);
        auto boundedH264 = motion::agent::video_transcode_rate_control(
            2560, 1440, 60, VideoTranscodeCodec::H264,
            130'000'000, 843'600'000);
        require(unboundedHevc.averageKbps == 14'377 &&
            unboundedHevc.maximumKbps == 21'565 &&
            unboundedHevc.maximumOutputBytes == 0,
            "the quality-oriented HEVC rate target changed unexpectedly");
        require(boundedHevc.averageKbps == 14'377 &&
            boundedHevc.maximumKbps == 17'822 &&
            boundedHevc.maximumOutputBytes == 194'000'000,
            "known source size/duration did not pre-bound HEVC max-rate to the acceptance budget");
        require(boundedH264.averageKbps == 16'040 &&
            boundedH264.maximumKbps == 17'822 &&
            boundedH264.maximumOutputBytes == boundedHevc.maximumOutputBytes &&
            boundedH264.bufferKbps == boundedH264.averageKbps * 2,
            "H.264 rate control can still encode a complete oversized file before rejection");
        auto hybrid = motion::agent::video_transcode_backend_order({
            VideoTranscodeAdapter{ 0x8086, 512ULL * 1024 * 1024, 1, 10, 11, true },
            VideoTranscodeAdapter{ 0x10de, 8ULL * 1024 * 1024 * 1024, 0, 20, 21, true }
        }, 2560, 1440, 120);
        require(hybrid.size() == 2 &&
            hybrid[0].backend == VideoTranscodeBackend::nvidiaNvenc &&
            hybrid[0].adapter.dxgiAdapterIndex == 0 && hybrid[0].adapter.luidHigh == 20 &&
            hybrid[1].backend == VideoTranscodeBackend::intelQsv &&
            hybrid[1].adapter.dxgiAdapterIndex == 1 && hybrid[1].adapter.luidHigh == 10,
            "hybrid GPU transcode order did not prefer the discrete adapter or bounded software work");

        auto dualNvidia = motion::agent::video_transcode_backend_order({
            VideoTranscodeAdapter{ 0x10de, 4ULL * 1024 * 1024 * 1024, 3, 30, 31, true },
            VideoTranscodeAdapter{ 0x10de, 12ULL * 1024 * 1024 * 1024, 2, 40, 41, true }
        }, 3840, 2160, 60);
        require(dualNvidia.size() == 2 &&
            dualNvidia[0].adapter.dxgiAdapterIndex == 2 &&
            dualNvidia[1].adapter.dxgiAdapterIndex == 3 &&
            dualNvidia[0].adapter.luidLow != dualNvidia[1].adapter.luidLow,
            "same-vendor GPUs collapsed into one unbound NVENC attempt");

        auto amd = motion::agent::video_transcode_backend_order({
            VideoTranscodeAdapter{ 0x1002, 4ULL * 1024 * 1024 * 1024, 4, 50, 51, true }
        }, 2560, 1440, 60);
        require(amd.size() == 2 && amd[0].backend == VideoTranscodeBackend::amdAmf &&
            amd[0].adapter.dxgiAdapterIndex == 4 &&
            amd[1].backend == VideoTranscodeBackend::softwareOpenH264,
            "AMD hardware encoding did not retain a broadly decodable software fallback");

        auto cpuOnly = motion::agent::video_transcode_backend_order({}, 1920, 1080, 60);
        require(cpuOnly.size() == 1 &&
            cpuOnly[0].backend == VideoTranscodeBackend::softwareOpenH264,
            "CPU-only systems lost their bounded H.264 software encoder fallback");
        require(motion::agent::video_transcode_backend_order({}, 1920, 1080, 60, true, false).empty(),
            "an HDR or high-bit-depth source was allowed through the 8-bit software encoder");
        require(motion::agent::video_transcode_backend_order({}, 3840, 2160, 60).empty(),
            "unsafe 4K software encoding was scheduled on a CPU-only system");

        auto cpuPlayback = motion::agent::video_transcode_backend_order(
            {}, 1920, 1080, 60, true, true, true);
        require(cpuPlayback.size() == 1 &&
            cpuPlayback[0].backend == VideoTranscodeBackend::softwareOpenH264,
            "CPU playback copy did not force the broadly decodable H.264 encoder");
        require(motion::agent::video_transcode_backend_order(
            {}, 1920, 1080, 60, true, false, true).empty(),
            "HDR or high-bit-depth video was destructively converted for CPU playback");

        auto unknownProbe = motion::agent::video_transcode_backend_order({}, 2560, 1440, 120, false);
        require(unknownProbe.empty(),
            "a failed LUID probe still guesses an unbound NVENC/QSV/AMF device");

        auto unknownSafeCpu = motion::agent::video_transcode_backend_order(
            {}, 1920, 1080, 60, false);
        require(unknownSafeCpu.size() == 1 &&
            unknownSafeCpu[0].backend == VideoTranscodeBackend::softwareOpenH264,
            "a failed LUID probe did not conservatively retain the bounded CPU fallback");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream transcoderFile(sourceRoot / L"MotionWallpaper.Agent" / L"VideoTranscoder.cpp",
            std::ios::binary);
        require(static_cast<bool>(transcoderFile), "VideoTranscoder source file was not found");
        std::string transcoder((std::istreambuf_iterator<char>(transcoderFile)), {});
        require(transcoder.find("L\"-init_hw_device\"") != std::string::npos &&
            transcoder.find("candidate.adapter.dxgiAdapterIndex") != std::string::npos &&
            transcoder.find("L\":,child_device=\"") != std::string::npos &&
            transcoder.find("child_device_type=d3d11va") != std::string::npos &&
            transcoder.find("current_dxgi_adapter_index(candidate.adapter)") !=
                std::string::npos &&
            transcoder.find("if (!adapter.identityKnown) return") != std::string::npos,
            "hardware candidates no longer bind FFmpeg to their concrete DXGI identity");
        require(transcoder.find("L\"-hwaccel\"") != std::string::npos &&
            transcoder.find("L\"-hwaccel_device\", deviceName") != std::string::npos &&
            transcoder.find("L\"-hwaccel_output_format\"") != std::string::npos &&
            transcoder.find("L\",hwdownload,format=\" + format") != std::string::npos &&
            transcoder.find("decodeAttempts = hardwareEncoder ? 2u : 1u") != std::string::npos,
            "bound hardware decode or its compatibility-decode retry was removed");
        require(transcoder.find("L\"-threads:v\", L\"4\"") != std::string::npos &&
            transcoder.find("L\"-filter_threads\", L\"2\"") != std::string::npos,
            "background software decode/scale parallelism is no longer bounded");
        require(transcoder.find("FurthestProcessedMicroseconds") != std::string::npos &&
            transcoder.find("lastPipeActivity") != std::string::npos &&
            transcoder.find("lastTimelineAdvance") != std::string::npos &&
            transcoder.find("stopAndConfirm(ERROR_TIMEOUT)") != std::string::npos &&
            transcoder.find("if (stalled) break") != std::string::npos,
            "a stalled hardware backend can once again block all fallback candidates forever");
    }

    struct FrameSchedulerProbe
    {
        static constexpr UINT message = WM_APP + 77;
        motion::renderer::FrameScheduler scheduler{ message };
        std::vector<std::chrono::steady_clock::time_point> ticks;

        static LRESULT CALLBACK WindowProc(HWND window, UINT event, WPARAM wParam, LPARAM lParam)
        {
            auto self = reinterpret_cast<FrameSchedulerProbe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (event == WM_NCCREATE) {
                auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<FrameSchedulerProbe*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self && event == message) {
                self->scheduler.TickHandled();
                self->ticks.push_back(std::chrono::steady_clock::now());
                // Approximate non-trivial frame transfer/presentation work.
                Sleep(3);
                if (self->ticks.size() < 30) self->scheduler.Start(window, 8);
                return 0;
            }
            return DefWindowProcW(window, event, wParam, lParam);
        }
    };

    void frame_scheduler_uses_real_interval()
    {
        require(motion::renderer::frame_due_time_100ns(25) == -250'000, "frame interval was not converted to a real deadline");
        require(motion::renderer::presentation_probe_interval_ms(333'333) == 24,
            "30 fps playback did not retain a pre-frame retry window");
        require(motion::renderer::presentation_probe_interval_ms(166'667) == 13,
            "60 fps playback wake-up is outside its low-power window");
        require(motion::renderer::presentation_probe_interval_ms(83'333) == 6,
            "high-frame-rate playback wake-up is too late");
        require(motion::renderer::presentation_probe_interval_ms(666'667) == 24,
            "a skipped frame escaped the bounded scheduling interval");
        motion::unique_handle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
        if (!timer) timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
        require(static_cast<bool>(timer), "waitable timer could not be created");
        LARGE_INTEGER due{};
        due.QuadPart = motion::renderer::frame_due_time_100ns(25);
        auto started = std::chrono::steady_clock::now();
        require(SetWaitableTimerEx(timer.get(), &due, 0, nullptr, nullptr, nullptr, 0) != FALSE, "frame timer could not be armed");
        require(WaitForSingleObject(timer.get(), 250) == WAIT_OBJECT_0, "frame timer did not fire");
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        require(elapsed >= std::chrono::milliseconds(18), "frame timer still fired at the old 1 ms cadence");
        require(elapsed < std::chrono::milliseconds(150), "frame timer deadline was excessively late");

        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = FrameSchedulerProbe::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MotionWallpaper.Tests.FrameScheduler";
        require(RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "frame scheduler probe window class failed");
        FrameSchedulerProbe probe;
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, windowClass.hInstance, &probe);
        require(window != nullptr, "frame scheduler probe window failed");
        std::atomic_uint64_t pressure{};
        std::jthread load([&](std::stop_token stop) {
            while (!stop.stop_requested()) pressure.fetch_add(1, std::memory_order_relaxed);
        });
        probe.scheduler.Start(window, 8);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (probe.ticks.size() < 30 && std::chrono::steady_clock::now() < deadline) {
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            Sleep(1);
        }
        probe.scheduler.Stop();
        load.request_stop();
        if (window) DestroyWindow(window);
        require(probe.ticks.size() == 30, "real FrameScheduler stalled under processing pressure");
        std::vector<int64_t> intervals;
        for (size_t index = 1; index < probe.ticks.size(); ++index) {
            intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                probe.ticks[index] - probe.ticks[index - 1]).count());
        }
        std::sort(intervals.begin(), intervals.end());
        require(intervals[intervals.size() / 2] >= 5 && intervals[intervals.size() / 2] <= 35,
            "real FrameScheduler median interval drifted outside its workload budget");
        require(intervals.back() < 100, "real FrameScheduler produced a visible long-frame stall");
    }

    void adapter_policy_preserves_heavy_video_throughput()
    {
        require(!motion::renderer::prefer_high_performance_adapter(1920, 1080, 60, 1),
            "ordinary 1080p video unnecessarily wakes the high-performance GPU");
        require(!motion::renderer::prefer_high_performance_adapter(3840, 2160, 30, 1),
            "ordinary 4K30 video unnecessarily wakes the high-performance GPU");
        require(motion::renderer::prefer_high_performance_adapter(3840, 2160, 60, 1),
            "4K60 video no longer receives the throughput-first adapter policy");
        require(motion::renderer::prefer_high_performance_adapter(3840, 2160, 240'000, 1'001),
            "high-frame-rate 4K video was assigned to the power-saving adapter");
        require(motion::renderer::prefer_high_performance_adapter(
            1920, 1080, 30, 1, 2ULL * 3840 * 2160),
            "multi-display composition pressure did not select the throughput adapter");
        require(!motion::renderer::prefer_high_performance_adapter(0, 2160, 60, 1),
            "invalid media metadata selected the high-performance adapter");
        require(motion::renderer::prefer_high_performance_adapter(
            1, 1, 60, 1, (std::numeric_limits<uint64_t>::max)() / 60 + 1),
            "extreme aggregate display load wrapped around to the low-power adapter policy");

        using motion::renderer::AdapterCandidate;
        AdapterCandidate discrete{ 0, 2, 8ULL * 1024 * 1024 * 1024 };
        AdapterCandidate integratedDisplay{ 1, 0, 0 };
        require(motion::renderer::adapter_candidate_precedes(
            discrete, integratedDisplay, true, true),
            "DXGI high-performance order was overridden by a muxless display attachment");
        require(motion::renderer::adapter_candidate_precedes(
            integratedDisplay, discrete, false, true),
            "a light workload unnecessarily selected the discrete GPU");
        require(motion::renderer::adapter_candidate_precedes(
            discrete, integratedDisplay, true, false),
            "legacy DXGI did not use dedicated memory as its vendor-neutral performance fallback");
    }

    void renderer_ack_channels_are_isolated()
    {
        auto target = motion::protocol::parse_ack("ack target 41 playing");
        auto control = motion::protocol::parse_ack("ack control 42 stopping");
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 41, "target ACK was not parsed");
        require(control.channel == motion::protocol::AckChannel::Control && control.revision == 42, "control ACK was not parsed");
        require(control.channel != motion::protocol::AckChannel::Target, "control ACK satisfied a target transition");
        require(motion::protocol::parse_ack("ack 43 playing").channel == motion::protocol::AckChannel::Unknown,
            "legacy ambiguous ACK was accepted");
        auto decode = motion::protocol::parse_decode_status(
            "status decode software-fallback fallback-no-hardware-decoder");
        require(decode.path == "software-fallback" && decode.reason == "fallback-no-hardware-decoder",
            "renderer decode status was not parsed");
        auto automatic = motion::protocol::parse_decode_status(
            "status decode automatic dxgi-manager-enabled");
        require(automatic.path == "automatic" && automatic.reason == "dxgi-manager-enabled",
            "automatic DXGI decode status was not parsed");
        require(motion::protocol::parse_decode_status("status decode hardware").path.empty(),
            "incomplete renderer decode status was accepted");
    }

    void decode_modes_have_distinct_fallback_contracts()
    {
        using motion::renderer::DecodePath;
        require(motion::renderer::select_decode_path(L"auto") == DecodePath::Automatic,
            "automatic decode no longer delegates selection to the DXGI-backed media engine");
        require(motion::renderer::select_decode_path(L"hardware") == DecodePath::Hardware,
            "explicit hardware decode no longer selects the physical GPU path");
        require(motion::renderer::select_decode_path(L"software") == DecodePath::Software,
            "explicit software decode no longer selects the WARP path");
        require(motion::renderer::allows_software_device_fallback(DecodePath::Automatic) &&
            !motion::renderer::allows_software_device_fallback(DecodePath::Hardware) &&
            !motion::renderer::allows_software_device_fallback(DecodePath::Software),
            "software-device fallback is no longer restricted to automatic decode");

        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream rendererFile(sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp",
            std::ios::binary);
        require(static_cast<bool>(rendererFile), "Renderer source file was not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        require(renderer.find("MF_MEDIA_ENGINE_DXGI_MANAGER") != std::string::npos,
            "video playback no longer supplies Media Engine with a DXGI device manager");
        require(renderer.find("MFTEnumEx(") == std::string::npos,
            "hardware-MFT enumeration was incorrectly reused as a DXVA capability probe");
    }

    std::string read_protocol_line(HANDLE pipe, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string pending;
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available) {
                char character{};
                DWORD read{};
                require(ReadFile(pipe, &character, 1, &read, nullptr) != FALSE,
                    "renderer protocol pipe failed");
                if (read && character == '\n') return pending;
                if (read) pending.push_back(character);
            } else {
                Sleep(10);
            }
        }
        return pending;
    }

    motion::protocol::Ack read_typed_ack(HANDLE pipe, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto ack = motion::protocol::parse_ack(read_protocol_line(pipe, remaining));
            if (ack.channel != motion::protocol::AckChannel::Unknown) return ack;
        }
        return {};
    }

    void write_test_bitmap(fs::path const& path)
    {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + 16;
        info.biSize = sizeof(info);
        info.biWidth = 2;
        info.biHeight = 2;
        info.biPlanes = 1;
        info.biBitCount = 24;
        info.biCompression = BI_RGB;
        info.biSizeImage = 16;
        std::array<unsigned char, 16> pixels{ 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 0, 0, 0 };
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<char const*>(&file), sizeof(file));
        output.write(reinterpret_cast<char const*>(&info), sizeof(info));
        output.write(reinterpret_cast<char const*>(pixels.data()), pixels.size());
    }

    struct RendererProcess
    {
        motion::unique_handle process;
        motion::unique_handle input;
        motion::unique_handle output;
        DWORD id{};

        void Send(std::string const& value) const
        {
            DWORD written{};
            require(WriteFile(input.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) != FALSE &&
                written == value.size(), "renderer command write failed");
        }
    };

    RendererProcess launch_hidden_image_renderer(fs::path const& root, std::wstring const& name)
    {
        auto renderer = motion::executable_directory().parent_path() / L"MotionWallpaper.App" / L"motionwallpaper-renderer.exe";
        if (!fs::is_regular_file(renderer)) {
            auto nativeRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
            renderer = nativeRoot / L"MotionWallpaper.Renderer" / L"x64" / L"Release" /
                L"MotionWallpaper.App" / L"motionwallpaper-renderer.exe";
        }
        require(fs::is_regular_file(renderer), "renderer executable was not built before integration tests");
        auto image = root / name;
        write_test_bitmap(image);

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        HANDLE inputReadRaw{}, inputWriteRaw{}, outputReadRaw{}, outputWriteRaw{};
        require(CreatePipe(&inputReadRaw, &inputWriteRaw, &security, 0) != FALSE, "renderer input pipe failed");
        motion::unique_handle inputRead(inputReadRaw), inputWrite(inputWriteRaw);
        require(CreatePipe(&outputReadRaw, &outputWriteRaw, &security, 0) != FALSE, "renderer output pipe failed");
        motion::unique_handle outputRead(outputReadRaw), outputWrite(outputWriteRaw);
        SetHandleInformation(inputWrite.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outputRead.get(), HANDLE_FLAG_INHERIT, 0);

        auto command = motion::build_command_line({ renderer.wstring(), L"-hidden", L"-video", image.wstring(),
            L"-kind", L"image", L"-decode", L"auto", L"-display", L"primary" });
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = inputRead.get();
        startup.hStdOutput = outputWrite.get();
        startup.hStdError = outputWrite.get();
        PROCESS_INFORMATION created{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, renderer.parent_path().c_str(), &startup, &created) != FALSE, "renderer integration process failed to launch");
        motion::unique_handle process(created.hProcess), processThread(created.hThread);
        inputRead.reset();
        outputWrite.reset();
        return { std::move(process), std::move(inputWrite), std::move(outputRead), created.dwProcessId };
    }

    void stop_renderer(RendererProcess& renderer, uint64_t revision)
    {
        renderer.Send("stop " + std::to_string(revision) + "\n");
        auto control = read_typed_ack(renderer.output.get(), std::chrono::seconds(2));
        require(control.channel == motion::protocol::AckChannel::Control &&
            control.revision == revision, "renderer did not acknowledge a clean integration-test stop");
        require(WaitForSingleObject(renderer.process.get(), 3000) == WAIT_OBJECT_0,
            "renderer integration process did not stop");
    }

    struct RendererWindowQuery
    {
        DWORD processId{};
        HWND window{};
    };

    BOOL CALLBACK find_renderer_window(HWND window, LPARAM parameter)
    {
        auto query = reinterpret_cast<RendererWindowQuery*>(parameter);
        DWORD processId{};
        GetWindowThreadProcessId(window, &processId);
        if (processId != query->processId) return TRUE;
        wchar_t className[128]{};
        if (GetClassNameW(window, className, ARRAYSIZE(className)) &&
            !_wcsicmp(className, L"MotionWallpaper.Native.Renderer")) {
            query->window = window;
            return FALSE;
        }
        return TRUE;
    }

    HWND wait_for_renderer_window(DWORD processId, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            RendererWindowQuery query{ processId };
            EnumWindows(find_renderer_window, reinterpret_cast<LPARAM>(&query));
            if (query.window) return query.window;
            if (std::chrono::steady_clock::now() >= deadline) break;
            Sleep(10);
        } while (true);
        return nullptr;
    }

    motion::protocol::Ack apply_and_wait_for_first_frame(
        RendererProcess const& renderer, std::string_view target, uint64_t revision)
    {
        auto started = std::chrono::steady_clock::now();
        renderer.Send(std::string(target) + " " + std::to_string(revision) + "\n");
        auto ack = read_typed_ack(renderer.output.get(), std::chrono::seconds(5));
        auto elapsed = std::chrono::steady_clock::now() - started;
        require(ack.channel == motion::protocol::AckChannel::Target &&
            ack.revision == revision && ack.state == "playing",
            "selected media did not reach a real Renderer first-frame ACK");
        require(elapsed < std::chrono::seconds(5),
            "selected media first-frame ACK exceeded the integration-test deadline");
        return ack;
    }

    void selected_media_reaches_real_renderer_first_frame(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"selection-first-frame.bmp");
        (void)apply_and_wait_for_first_frame(renderer, "desktop-play", 101);
        stop_renderer(renderer, 102);
    }

    void renderer_crash_recovery_reaches_first_frame_again(fs::path const& root)
    {
        auto crashed = launch_hidden_image_renderer(root, L"crash-recovery.bmp");
        (void)apply_and_wait_for_first_frame(crashed, "desktop-play", 201);
        auto crashedProcessId = crashed.id;
        require(TerminateProcess(crashed.process.get(), 0xDEAD) != FALSE,
            "test-owned Renderer could not be crashed");
        require(WaitForSingleObject(crashed.process.get(), 3000) == WAIT_OBJECT_0,
            "crashed Renderer did not terminate");
        crashed.input.reset();
        crashed.output.reset();

        auto recovered = launch_hidden_image_renderer(root, L"crash-recovery.bmp");
        require(recovered.id != crashedProcessId,
            "Renderer recovery reused the terminated process unexpectedly");
        (void)apply_and_wait_for_first_frame(recovered, "desktop-play", 202);
        stop_renderer(recovered, 203);
    }

    void renderer_display_change_exit_and_relaunch_is_cross_process(fs::path const& root)
    {
        auto beforeChange = launch_hidden_image_renderer(root, L"display-change.bmp");
        (void)apply_and_wait_for_first_frame(beforeChange, "desktop-play", 301);
        auto window = wait_for_renderer_window(beforeChange.id, std::chrono::seconds(3));
        require(window != nullptr, "real Renderer window was not discoverable for display-change injection");
        require(PostMessageW(window, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080)) != FALSE,
            "display-change message could not be delivered to the real Renderer");
        require(WaitForSingleObject(beforeChange.process.get(), 3000) == WAIT_OBJECT_0,
            "Renderer did not retire its stale display route after WM_DISPLAYCHANGE");
        beforeChange.input.reset();
        beforeChange.output.reset();

        auto afterChange = launch_hidden_image_renderer(root, L"display-change.bmp");
        (void)apply_and_wait_for_first_frame(afterChange, "desktop-play", 302);
        stop_renderer(afterChange, 303);
    }

    void real_renderer_enters_screensaver_and_returns_on_wake(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"screensaver-wake.bmp");
        renderer.Send("pause 400\n");
        auto desktopReady = read_typed_ack(renderer.output.get(), std::chrono::seconds(2));
        require(desktopReady.channel == motion::protocol::AckChannel::Target &&
            desktopReady.revision == 400 && desktopReady.state == "paused",
            "real Renderer did not establish its desktop state before screen-saver entry");
        (void)apply_and_wait_for_first_frame(renderer, "screensaver-play", 401);

        // The Agent maps the first raw input event to the desktop pause target;
        // use the real cross-process target transition after the event-policy
        // test below has verified that raw input increments the wake revision.
        renderer.Send("pause 402\n");
        auto wake = read_typed_ack(renderer.output.get(), std::chrono::seconds(2));
        require(wake.channel == motion::protocol::AckChannel::Target &&
            wake.revision == 402 && wake.state == "paused",
            "screen saver did not acknowledge its immediate wake target");
        stop_renderer(renderer, 403);
    }

    struct RuntimeEventMessageProbe
    {
        uint32_t taskbarCreated{};
        uint64_t topologyRevision{};
        uint64_t inputRevision{};
        uint32_t shellRestarts{};
        bool displayOn{};
        bool rawInputWakeEnabled{};

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            auto self = reinterpret_cast<RuntimeEventMessageProbe*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<RuntimeEventMessageProbe*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (!self) return DefWindowProcW(window, message, wParam, lParam);
            auto effect = motion::agent::runtime_system_event_effect(
                message, wParam, self->taskbarCreated, self->rawInputWakeEnabled);
            motion::agent::apply_runtime_system_event_effect(effect,
                self->topologyRevision, self->inputRevision, self->displayOn);
            if (effect.shellRestarted) ++self->shellRestarts;
            if (effect.handled || message == WM_INPUT) return 0;
            return DefWindowProcW(window, message, wParam, lParam);
        }
    };

    void safe_agent_system_event_messages_trigger_recovery()
    {
        WNDCLASSW definition{};
        definition.lpfnWndProc = RuntimeEventMessageProbe::WindowProc;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"MotionWallpaper.Tests.RuntimeEvents";
        require(RegisterClassW(&definition) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "runtime-event simulation window class failed");

        RuntimeEventMessageProbe probe;
        probe.taskbarCreated = RegisterWindowMessageW(
            L"MotionWallpaper.Tests.TaskbarCreated.Resilience");
        require(probe.taskbarCreated != 0, "Explorer-restart simulation message was not registered");
        HWND window = CreateWindowExW(0, definition.lpszClassName, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, definition.hInstance, &probe);
        require(window != nullptr, "runtime-event simulation window failed");

        SendMessageW(window, WM_INPUT, 0, 0);
        require(probe.inputRevision == 0,
            "raw input woke a screen saver while event wake was disabled");
        probe.rawInputWakeEnabled = true;
        SendMessageW(window, WM_INPUT, 0, 0);
        require(probe.inputRevision == 1,
            "enabled raw input did not synchronously advance the wake revision");

        probe.displayOn = false;
        SendMessageW(window, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        require(probe.displayOn && probe.topologyRevision == 1,
            "automatic sleep resume did not restore display state and invalidate Renderer routes");
        SendMessageW(window, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
        require(probe.topologyRevision == 2,
            "interactive sleep resume did not invalidate Renderer routes");

        SendMessageW(window, probe.taskbarCreated, 0, 0);
        require(probe.topologyRevision == 3 && probe.shellRestarts == 1,
            "Explorer restart did not recreate the tray/topology generation");
        SendMessageW(window, WM_DISPLAYCHANGE, 32, MAKELPARAM(2560, 1440));
        require(probe.topologyRevision == 4,
            "display hot-plug did not invalidate Renderer routes");
        DestroyWindow(window);
    }

    void screensaver_input_wake_is_immediate_in_runtime_state_machine()
    {
        motion::Settings settings;
        settings.screensaverEnabled = true;
        settings.idleTimeoutSeconds = 30;
        settings.desktopPlayback = true;
        settings.activePlaybackEnabled = true;
        motion::IdleTimer idle;
        auto screenSaverIdle = idle.Update(std::chrono::seconds(60), 100,
            std::chrono::seconds(30), false);
        require(motion::agent::reduce_runtime_action(settings,
            { true, false, false, true, screenSaverIdle.count() / 1000 }) ==
                motion::agent::RuntimeAction::ScreensaverPlay,
            "idle state did not enter the screen saver at its configured boundary");

        motion::agent::TrayControlState preview;
        preview.RequestScreensaverPreview(100, 0);
        uint64_t topologyRevision{};
        uint64_t inputRevision{};
        bool displayOn = true;
        auto rawInput = motion::agent::runtime_system_event_effect(
            WM_INPUT, 0, 0, true);
        motion::agent::apply_runtime_system_event_effect(rawInput,
            topologyRevision, inputRevision, displayOn);
        require(preview.ObserveInput(100, inputRevision) &&
            !preview.ScreensaverPreviewActive(),
            "raw input did not end instant screen-saver preview in the same event turn");

        auto activeIdle = idle.Update(std::chrono::seconds(60) +
            std::chrono::milliseconds(1), 101, std::chrono::milliseconds::zero(), false);
        require(activeIdle == std::chrono::milliseconds::zero() &&
            motion::agent::reduce_runtime_action(settings,
                { true, false, false, true, activeIdle.count() / 1000 }) ==
                    motion::agent::RuntimeAction::DesktopPlay,
            "mouse activity did not transition the screen saver directly back to desktop playback");
    }

    void renderer_process_uses_typed_acks(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"protocol.bmp");
        renderer.Send("desktop-play 1\n");
        auto target = read_typed_ack(renderer.output.get(), std::chrono::seconds(5));
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 1, "renderer did not emit a typed target ACK");
        renderer.Send("legacy-play 8\n");
        auto error = read_protocol_line(renderer.output.get(), std::chrono::seconds(2));
        require(error.starts_with("error 8 invalid-command"), "renderer accepted a removed protocol alias");
        renderer.Send("stop 2\n");
        auto control = read_typed_ack(renderer.output.get(), std::chrono::seconds(2));
        require(control.channel == motion::protocol::AckChannel::Control && control.revision == 2, "renderer did not emit a typed control ACK");
        require(WaitForSingleObject(renderer.process.get(), 3000) == WAIT_OBJECT_0, "renderer did not stop after the stop command");
    }

    void renderer_exits_when_agent_pipe_closes(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"protocol-eof.bmp");
        renderer.Send("desktop-play 11\n");
        auto target = read_typed_ack(renderer.output.get(), std::chrono::seconds(5));
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 11,
            "renderer was not ready before the EOF test");
        renderer.input.reset();
        require(WaitForSingleObject(renderer.process.get(), 3000) == WAIT_OBJECT_0,
            "renderer survived after its Agent command pipe closed");
    }

    motion::unique_handle launch_writer_process(std::wstring_view mode, fs::path const& path)
    {
        auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
        std::wstring command = L"\"" + executable.wstring() + L"\" " + std::wstring(mode) + L" \"" + path.wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION created{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &created) != FALSE,
            "configuration writer process failed to launch");
        CloseHandle(created.hThread);
        return motion::unique_handle(created.hProcess);
    }

    void performance_copy_queue_is_observable_across_processes(fs::path const& root)
    {
        auto mediaDirectory = root / L"performance-copy-cross-process";
        fs::create_directories(mediaDirectory);
        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "selected wallpaper could not enqueue a durable performance copy");
        auto expected = motion::read_variant_generation_request(mediaDirectory);
        require(expected && !expected.requestId.empty(),
            "performance-copy queue did not publish an ABA-safe request");

        auto worker = launch_writer_process(L"--run-variant-worker", mediaDirectory);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        motion::VariantCacheStatus status;
        do {
            status = motion::inspect_variant_cache(mediaDirectory);
            if (status.generating && status.progressKnown) break;
            require(WaitForSingleObject(worker.get(), 0) == WAIT_TIMEOUT,
                "performance-copy worker exited before publishing progress");
            Sleep(10);
        } while (std::chrono::steady_clock::now() < deadline);
        require(status.queued && status.generating && status.progressKnown &&
            status.progressPercent == 35 && status.estimatedRemainingKnown &&
            status.estimatedRemainingSeconds == 4,
            "another process could not observe honest performance-copy progress and ETA");
        require(motion::write_small_file(mediaDirectory / L".test-worker-continue", "continue"),
            "performance-copy test worker could not be released");
        require(WaitForSingleObject(worker.get(), 5000) == WAIT_OBJECT_0,
            "performance-copy worker did not complete its queued request");
        DWORD exitCode{};
        require(GetExitCodeProcess(worker.get(), &exitCode) && exitCode == 0,
            "performance-copy worker rejected the durable queue request");
        status = motion::inspect_variant_cache(mediaDirectory);
        require(!status.queued && !status.generating &&
            !fs::exists(motion::variant_progress_path(mediaDirectory)),
            "completed performance-copy work remained queued or exposed stale progress");
    }

    void settings_and_runtime_have_single_writers(fs::path const& root)
    {
        auto settingsPath = root / L"single-writer" / L"settings.json";
        auto runtimePath = root / L"single-writer" / L"runtime.json";
        auto settingsWriter = launch_writer_process(L"--write-settings", settingsPath);
        auto runtimeWriter = launch_writer_process(L"--write-runtime", runtimePath);
        HANDLE writers[]{ settingsWriter.get(), runtimeWriter.get() };
        require(WaitForMultipleObjects(2, writers, TRUE, 10'000) == WAIT_OBJECT_0,
            "configuration writer processes timed out");
        DWORD settingsExit{}, runtimeExit{};
        require(GetExitCodeProcess(settingsWriter.get(), &settingsExit) && settingsExit == 0,
            "settings writer process failed");
        require(GetExitCodeProcess(runtimeWriter.get(), &runtimeExit) && runtimeExit == 0,
            "runtime writer process failed");

        auto loadedSettings = motion::load_settings(settingsPath);
        auto loadedRuntime = motion::load_runtime(runtimePath);
        require(loadedSettings && loadedSettings->idleTimeoutSeconds == 777 &&
            loadedSettings->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
            "runtime publication overwrote user settings");
        require(loadedRuntime && loadedRuntime->activeMediaId == "cccccccc-cccc-cccc-cccc-cccccccccccc" &&
            loadedRuntime->decodePath == "software-fallback" &&
            loadedRuntime->decodeReason == "fallback-no-hardware-decoder",
            "settings publication overwrote runtime selection");
    }

    void automatic_decode_runtime_round_trips(fs::path const& root)
    {
        auto path = root / L"automatic-runtime.json";
        motion::RuntimeState runtime;
        runtime.activeGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        runtime.activeMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        runtime.decodePath = "automatic";
        runtime.decodeReason = "dxgi-manager-enabled";
        motion::save_runtime(path, runtime);

        auto loaded = motion::load_runtime(path);
        require(loaded && loaded->decodePath == "automatic" &&
            loaded->decodeReason == "dxgi-manager-enabled",
            "automatic DXGI decode status did not survive runtime publication");
    }

    void display_runtime_status_and_control_round_trip(fs::path const& root)
    {
        auto runtimePath = root / L"display-runtime" / L"runtime.json";
        motion::RuntimeState runtime;
        runtime.activeGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        runtime.activeMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        runtime.decodePath = "software-fallback";
        runtime.decodeReason = "fallback-no-hardware-decoder";
        runtime.agentInstanceId = "dddddddd-dddd-dddd-dddd-dddddddddddd";
        runtime.agentProcessId = 4321;
        runtime.displayStates = {
            { "DISPLAY#ONE", L"\\\\.\\DISPLAY1", L"Internal display",
                runtime.activeGroupId, runtime.activeMediaId, "degraded",
                "fallback-no-hardware-decoder", "software-fallback",
                "fallback-no-hardware-decoder", 1234, false, true },
            { "DISPLAY#TWO", L"\\\\.\\DISPLAY2", L"External display",
                runtime.activeGroupId, runtime.activeMediaId, "optimizing",
                "performance-copy-pending", "probing", "detecting", 2345, false, true }
        };
        runtime.lastCommandId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        runtime.lastCommandAction = "restart-renderer";
        runtime.lastCommandSucceeded = true;
        runtime.lastCommandMessage = "renderer-restart-scheduled";
        motion::save_runtime(runtimePath, runtime);

        auto loaded = motion::load_runtime(runtimePath);
        require(loaded && loaded->version == motion::runtime_schema_version &&
            loaded->displayStates == runtime.displayStates &&
            loaded->agentInstanceId == runtime.agentInstanceId &&
            loaded->agentProcessId == runtime.agentProcessId &&
            loaded->lastCommandId == runtime.lastCommandId &&
            loaded->lastCommandAction == "restart-renderer" &&
            loaded->lastCommandSucceeded &&
            loaded->lastCommandMessage == "renderer-restart-scheduled",
            "per-display runtime status or Renderer command acknowledgement did not round-trip");

        auto legacyPath = root / L"display-runtime" / L"legacy-runtime.json";
        std::ofstream(legacyPath, std::ios::binary | std::ios::trunc) <<
            R"({"version":1,"activeGroupId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","activeMediaId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","decodePath":"hardware","decodeReason":"","updatedAt":""})";
        auto legacy = motion::load_runtime(legacyPath);
        require(legacy && legacy->version == motion::runtime_schema_version &&
            legacy->displayStates.empty() && legacy->lastCommandId.empty(),
            "runtime v1 compatibility was lost when per-display status was added");

        auto controlPath = root / L"display-runtime" / L"runtime-command.json";
        auto requestId = motion::request_runtime_control(
            controlPath, "retry", "DISPLAY#TWO");
        auto request = motion::load_runtime_control_request(controlPath);
        require(requestId && request && request->requestId == *requestId &&
            request->action == "retry" && request->displayId == "DISPLAY#TWO" &&
            !request->createdAt.empty(),
            "a targeted Renderer retry request was not durably published");
        require(!motion::request_runtime_control(controlPath, "delete-renderer", {}) &&
            motion::load_runtime_control_request(controlPath)->requestId == *requestId,
            "an invalid runtime command was accepted or replaced a valid request");
    }

    void display_runtime_status_has_trustworthy_precedence()
    {
        using motion::agent::DisplayRuntimeSignals;
        using motion::agent::display_runtime_state;
        require(display_runtime_state({}) == "applying",
            "a Renderer without a first-frame acknowledgement appeared applied");
        require(display_runtime_state({ false, false, false, true, false }) == "applied",
            "an acknowledged Renderer did not appear applied");
        require(display_runtime_state({ false, false, false, true, true }) == "degraded",
            "a live compatibility fallback was not surfaced as degraded");
        require(display_runtime_state({ false, true, false, true, true }) == "optimizing",
            "selected-media optimization was hidden behind the old playback route");
        require(display_runtime_state({ false, true, false, false, true }) == "applying",
            "cold-start optimization appeared active before the first-frame acknowledgement");
        require(display_runtime_state({ false, false, true, true, false }) == "paused",
            "an intentional playback pause was not distinguishable from applying");
        require(display_runtime_state({ false, false, true, false, false }) == "applying",
            "a pause was published before Renderer acknowledged its target");
        require(display_runtime_state({ true, true, false, false, false }) == "failed",
            "a Renderer failure was hidden behind optimization progress");
        require(motion::agent::desktop_pause_reason(true) == "manual-pause" &&
            motion::agent::desktop_pause_reason(false) == "desktop-covered",
            "manual pause is still reported as desktop coverage");
    }

    void agent_consumes_renderer_recovery_commands()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp",
            std::ios::binary);
        require(static_cast<bool>(agentFile), "Agent source file was not found");
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        require(agent.find("try_load_runtime_control_request(") != std::string::npos &&
            agent.find("renderers.Retry(controlRequest.displayId)") != std::string::npos &&
            agent.find("renderers.Restart(controlRequest.displayId)") != std::string::npos &&
            agent.find("RuntimeStates(displays, outputs") != std::string::npos &&
            agent.find("lastCommandSucceeded = succeeded") != std::string::npos,
            "Agent no longer consumes, targets, and acknowledges Renderer recovery commands");
    }

    void concurrent_settings_writers_never_publish_torn_json(fs::path const& root)
    {
        auto path = root / L"concurrent-settings" / L"settings.json";
        auto first = launch_writer_process(L"--write-settings-a", path);
        auto second = launch_writer_process(L"--write-settings-b", path);
        HANDLE writers[]{ first.get(), second.get() };
        require(WaitForMultipleObjects(2, writers, TRUE, 10'000) == WAIT_OBJECT_0,
            "concurrent settings writers timed out");
        DWORD firstExit{}, secondExit{};
        if (!GetExitCodeProcess(first.get(), &firstExit) || !GetExitCodeProcess(second.get(), &secondExit) ||
            firstExit != 0 || secondExit != 0) {
            throw std::runtime_error("a concurrent settings writer failed (" +
                std::to_string(firstExit) + ", " + std::to_string(secondExit) + ")");
        }
        auto loaded = motion::load_settings(path);
        require(loaded.has_value(), "concurrent settings writes published torn JSON");
        bool firstRecord = loaded->idleTimeoutSeconds == 701 &&
            loaded->selectedMediaId == "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        bool secondRecord = loaded->idleTimeoutSeconds == 702 &&
            loaded->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        require(firstRecord || secondRecord,
            "concurrent settings writes combined fields from different revisions");
    }

    void corrupt_files_preserve_last_known_good(fs::path const& root)
    {
        auto config = root / L"corrupt-settings.json";
        std::ofstream(config) << "{broken";
        motion::Settings lastKnownGood;
        lastKnownGood.idleTimeoutSeconds = 321;
        require(!motion::try_load_settings(config, lastKnownGood), "corrupt settings were accepted");
        require(lastKnownGood.idleTimeoutSeconds == 321, "corrupt settings destroyed the last-known-good state");

        auto metadata = root / L"corrupt-media.json";
        std::ofstream(metadata) << "not-json";
        motion::MediaMetadata media;
        require(!motion::try_load_media(metadata, media), "corrupt media metadata escaped resilient loading");
    }

    void media_library_mutations_revalidate_persistent_identity(fs::path const& root)
    {
        auto libraryRoot = fs::absolute(root / L"guarded-media-library");
        create_owned_library_root(libraryRoot, testLibraryId);
        auto identity = motion::capture_media_library_trust(libraryRoot);
        require(identity.has_value(), "guarded media library identity could not be captured");
        motion::app::MediaLibrary library(root / L"guarded-app-data",
            motion::app::DeleteMode::Permanent, libraryRoot, *identity);
        auto group = library.CreateGroup(L"guarded", {});
        auto sentinel = libraryRoot / L"Groups" / motion::utf8_to_wide(group.id) / L"keep.txt";
        std::ofstream(sentinel, std::ios::binary) << "replacement data";

        fs::remove(libraryRoot / motion::media_library_ownership_marker_name);
        std::ofstream(libraryRoot / motion::media_library_ownership_marker_name, std::ios::binary)
            << motion::media_library_ownership_marker_prefix << replacementLibraryId << '\n';
        bool deleteRejected{};
        try { library.DeleteGroup(group); }
        catch (...) { deleteRejected = true; }
        require(deleteRejected && fs::is_regular_file(sentinel),
            "MediaLibrary deleted through a path after its persisted identity changed");

        auto importRoot = fs::absolute(root / L"guarded-import-library");
        create_owned_library_root(importRoot, testLibraryId);
        auto importIdentity = motion::capture_media_library_trust(importRoot);
        require(importIdentity.has_value(), "guarded import identity could not be captured");
        motion::app::MediaLibrary importLibrary(root / L"guarded-import-data",
            motion::app::DeleteMode::Permanent, importRoot, *importIdentity);
        auto importGroup = importLibrary.CreateGroup(L"import", {});
        auto source = root / L"guarded-import-source.bmp";
        write_test_bitmap(source);
        bool identityChanged{};
        bool importRejected{};
        fs::path replacementSentinel;
        try {
            (void)importLibrary.Import(source, "image", importGroup.id,
                [&](uint64_t, uint64_t) {
                    if (identityChanged) return;
                    auto videos = importRoot / L"Groups" /
                        motion::utf8_to_wide(importGroup.id) / L"Videos";
                    for (auto const& entry : fs::directory_iterator(videos)) {
                        if (!entry.is_directory()) continue;
                        replacementSentinel = entry.path() / L"replacement-sentinel.txt";
                        std::ofstream(replacementSentinel, std::ios::binary) << "do not clean by path";
                        break;
                    }
                    fs::remove(importRoot / motion::media_library_ownership_marker_name);
                    std::ofstream(importRoot / motion::media_library_ownership_marker_name,
                        std::ios::binary) << motion::media_library_ownership_marker_prefix
                        << replacementLibraryId << '\n';
                    identityChanged = true;
                });
        } catch (...) {
            importRejected = true;
        }
        require(identityChanged && importRejected && !replacementSentinel.empty() &&
            fs::is_regular_file(replacementSentinel),
            "Import published or path-cleaned staging after the external library identity changed");
    }

    void interrupted_variant_deletion_is_recovered(fs::path const& root)
    {
        auto libraryRoot = fs::absolute(root / L"vdr");
        create_owned_library_root(libraryRoot, testLibraryId);
        auto identity = motion::capture_media_library_trust(libraryRoot);
        require(identity.has_value(), "variant recovery library identity could not be captured");
        motion::app::MediaLibrary library(root / L"vdd",
            motion::app::DeleteMode::Permanent, libraryRoot, *identity);
        auto group = library.CreateGroup(L"recovery", {});

        motion::MediaMetadata media;
        media.id = "aaaaaaaa";
        media.groupId = group.id;
        media.name = L"recoverable";
        media.originalName = L"source.mp4";
        media.fileName = L"source.mp4";
        media.kind = "video";
        media.importedAt = media.updatedAt = motion::timestamp_utc();
        auto mediaDirectory = libraryRoot / L"Groups" / motion::utf8_to_wide(group.id) /
            L"Videos" / motion::utf8_to_wide(media.id);
        auto tombstone = mediaDirectory / L"Variants" /
            L".deleting-bbbbbbbb";
        fs::create_directories(tombstone);
        std::ofstream(mediaDirectory / media.fileName, std::ios::binary) << "source";
        motion::save_media(mediaDirectory / L"metadata.json", media);
        auto variantName = L"balanced-60-1280x720-v5.mp4";
        std::ofstream(tombstone / variantName, std::ios::binary) << "retained variant";

        auto status = library.VariantStatus(media);
        require(fs::is_regular_file(mediaDirectory / L"Variants" / variantName) &&
            !fs::exists(tombstone) && status.entries.size() == 1 &&
            status.entries.front().fileName == variantName,
            "an interrupted pre-suppression variant deletion remained hidden after recovery");

        auto variants = mediaDirectory / L"Variants";
        auto duplicateTombstone = variants / L".deleting-cccccccc";
        fs::create_directory(duplicateTombstone);
        std::ofstream(duplicateTombstone / variantName, std::ios::binary)
            << "retained variant";
        auto duplicateStatus = library.VariantStatus(media);
        require(!fs::exists(duplicateTombstone) &&
            fs::is_regular_file(variants / variantName) &&
            duplicateStatus.entries.size() == 1,
            "an identical staged duplicate was not safely collapsed during recovery");

        auto conflictTombstone = variants / L".deleting-dddddddd";
        auto conflictDirectory = variants / L".conflict-dddddddd";
        fs::create_directory(conflictTombstone);
        std::ofstream(conflictTombstone / variantName, std::ios::binary)
            << "different staged variant";
        auto firstConflictStatus = library.VariantStatus(media);
        require(firstConflictStatus.entries.empty() && !fs::exists(conflictTombstone) &&
            fs::is_regular_file(conflictDirectory / variantName),
            "a differing staged duplicate was not quarantined for manual recovery");

        auto readSmallFile = [](fs::path const& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(input)), {});
        };
        require(readSmallFile(variants / variantName) == "retained variant" &&
            readSmallFile(conflictDirectory / variantName) == "different staged variant",
            "variant-conflict recovery overwrote one of the distinct copies");
        auto secondConflictStatus = library.VariantStatus(media);
        require(secondConflictStatus.entries.size() == 1 &&
            fs::is_regular_file(conflictDirectory / variantName),
            "a quarantined variant conflict was retried or re-reported indefinitely");
    }

    void interrupted_library_transactions_recover_without_overwrite(
        fs::path const& root)
    {
        auto libraryRoot = fs::absolute(root / L"transaction-recovery-library");
        create_owned_library_root(libraryRoot, testLibraryId);
        auto identity = motion::capture_media_library_trust(libraryRoot);
        require(identity.has_value(), "transaction recovery identity was not captured");
        motion::app::MediaLibrary setup(root / L"transaction-recovery-data",
            motion::app::DeleteMode::Permanent, libraryRoot, *identity);
        auto sourceGroup = setup.CreateGroup(L"source", {});
        auto targetGroup = setup.CreateGroup(L"target", { sourceGroup });

        motion::MediaMetadata media;
        media.id = "aaaaaaaa";
        media.groupId = sourceGroup.id;
        media.name = media.originalName = L"source.mp4";
        media.fileName = L"source.mp4";
        media.kind = "video";
        media.revision = 1;
        media.importedAt = media.updatedAt = motion::timestamp_utc();
        auto source = libraryRoot / L"Groups" /
            motion::utf8_to_wide(sourceGroup.id) / L"Videos" /
            motion::utf8_to_wide(media.id);
        auto targetVideos = libraryRoot / L"Groups" /
            motion::utf8_to_wide(targetGroup.id) / L"Videos";
        fs::create_directories(source);
        fs::create_directories(targetVideos);
        motion::save_media(source / L"metadata.json", media);
        std::ofstream(source / media.fileName, std::ios::binary) << "source";

        constexpr wchar_t moveToken[] = L"0123456789abcdef0123";
        auto moveStage = targetVideos / (std::wstring(L".mw-moving-") + moveToken);
        auto moveRecord = targetVideos / (std::wstring(L".mw-move-") + moveToken);
        std::ofstream(moveRecord, std::ios::binary)
            << "MotionWallpaper.Move/v1\n" << testLibraryId << '\n'
            << media.id << '\n' << sourceGroup.id << '\n' << targetGroup.id << '\n';
        fs::rename(source, moveStage);
        {
            motion::app::MediaLibrary recovery(root / L"transaction-recovery-data",
                motion::app::DeleteMode::Permanent, libraryRoot, *identity);
            (void)recovery.LoadGroups();
        }
        auto restored = motion::load_media(source / L"metadata.json");
        require(restored && restored->groupId == sourceGroup.id &&
            !fs::exists(moveStage) && !fs::exists(moveRecord),
            "an interrupted group move was not deterministically rolled back");

        constexpr wchar_t conflictToken[] = L"fedcba9876543210fedc";
        auto conflictStage = targetVideos /
            (std::wstring(L".mw-moving-") + conflictToken);
        auto conflictRecord = targetVideos /
            (std::wstring(L".mw-move-") + conflictToken);
        fs::create_directories(conflictStage);
        motion::save_media(conflictStage / L"metadata.json", media);
        std::ofstream(conflictRecord, std::ios::binary)
            << "MotionWallpaper.Move/v1\n" << testLibraryId << '\n'
            << media.id << '\n' << sourceGroup.id << '\n' << targetGroup.id << '\n';
        {
            motion::app::MediaLibrary recovery(root / L"transaction-recovery-data",
                motion::app::DeleteMode::Permanent, libraryRoot, *identity);
            (void)recovery.LoadGroups();
            (void)recovery.LoadGroups();
        }
        require(fs::is_directory(source) &&
            fs::is_directory(targetVideos /
                (std::wstring(L".mw-move-conflict-") + conflictToken)) &&
            fs::is_regular_file(targetVideos /
                (std::wstring(L".mw-move-conflict-") + conflictToken + L".record")) &&
            !fs::exists(conflictRecord),
            "a move collision overwrote data or remained in an automatic retry loop");

        constexpr wchar_t deleteToken[] = L"00112233445566778899";
        auto original = source / L"recover-me.bin";
        auto deleteStage = source / (std::wstring(L".mw-delete-") + deleteToken);
        auto deleteRecord = source / (std::wstring(L".mw-restore-") + deleteToken);
        std::ofstream(original, std::ios::binary) << "recover";
        std::ofstream(deleteRecord, std::ios::binary)
            << "MotionWallpaper.Delete/v1\n" << testLibraryId
            << "\nrecover-me.bin\n";
        fs::rename(original, deleteStage);
        {
            motion::app::MediaLibrary recovery(root / L"transaction-recovery-data",
                motion::app::DeleteMode::Permanent, libraryRoot, *identity);
            (void)recovery.LoadGroups();
        }
        require(fs::is_regular_file(original) && !fs::exists(deleteStage) &&
            !fs::exists(deleteRecord),
            "an interrupted stable-path recycle transaction was not restored");
    }

    void media_library_operations_are_safe(fs::path const& root)
    {
        char const* phase = "initialize";
        try {
            auto libraryRoot = root / L"library";
            motion::app::MediaLibrary library(libraryRoot, motion::app::DeleteMode::Permanent);
            library.EnsureDirectories();
            phase = "create groups";
            auto first = library.CreateGroup(L"海景", {});
            auto second = library.CreateGroup(L"山谷", { first });

            phase = "reject disguised content";
            auto disguisedVideo = root / L"disguised.mp4";
            std::ofstream(disguisedVideo, std::ios::binary) << "not a media container";
            bool rejected{};
            try { (void)library.Import(disguisedVideo, "video", first.id); }
            catch (...) { rejected = true; }
            require(rejected, "extension-only validation accepted disguised video content");

            phase = "import";
            auto source = root / L"sample.png";
            write_test_bitmap(source);
            auto mediaId = library.Import(source, "image", first.id);
            auto imported = library.LoadMedia(first.id);
            require(imported.size() == 1 && imported.front().id == mediaId, "media import did not round-trip");
            require(imported.front().coverFileName.empty(),
                "an image import exposed the full-resolution source as a UI thumbnail");
            phase = "lightweight image cover";
            require(library.EnsureCover(imported.front()),
                "a bounded image cover could not be generated");
            imported = library.LoadMedia(first.id);
            require(imported.size() == 1 && imported.front().coverFileName == L"poster.png" &&
                imported.front().coverFileName != imported.front().fileName &&
                fs::is_regular_file(library.MediaDirectory(imported.front()) / L"poster.png"),
                "the image source was not separated from its lightweight UI cover");
            phase = "concurrent cover and rename";
            fs::copy_file(source, library.MediaDirectory(imported.front()) / L"poster.png",
                fs::copy_options::overwrite_existing);
            std::exception_ptr renameError, coverError;
            std::thread rename([&] {
                try { library.Rename(imported.front(), L"清晨海景"); } catch (...) { renameError = std::current_exception(); }
            });
            std::thread cover([&] {
                try { library.UpdateCover(imported.front(), L"poster.png"); } catch (...) { coverError = std::current_exception(); }
            });
            rename.join();
            cover.join();
            if (renameError) std::rethrow_exception(renameError);
            if (coverError) std::rethrow_exception(coverError);
            auto concurrentlyUpdated = library.LoadMedia(first.id);
            if (concurrentlyUpdated.size() != 1 || concurrentlyUpdated.front().name != L"清晨海景" ||
                concurrentlyUpdated.front().coverFileName != L"poster.png" || concurrentlyUpdated.front().revision < 3) {
                auto revision = concurrentlyUpdated.empty() ? 0 : concurrentlyUpdated.front().revision;
                auto coverName = concurrentlyUpdated.empty() ? std::wstring{} : concurrentlyUpdated.front().coverFileName;
                throw std::runtime_error("serialized media operations lost a field update (revision " +
                    std::to_string(revision) + ", cover " + motion::wide_to_utf8(coverName) + ")");
            }
            phase = "move";
            // Intentionally use the stale pre-rename record. Operations resolve
            // by media ID and must not overwrite the latest metadata revision.
            library.Move(imported.front(), second.id);
            require(library.LoadMedia(first.id).empty(), "moved media survived in the source group");
            auto moved = library.LoadMedia(second.id);
            require(moved.size() == 1 && moved.front().groupId == second.id, "moved media was not committed to the target group");
            library.Rename(imported.front(), L"山谷清晨");
            moved = library.LoadMedia(second.id);
            require(moved.size() == 1 && moved.front().name == L"山谷清晨" && moved.front().coverFileName == L"poster.png",
                "a stale asynchronous operation did not follow the moved media safely");
            phase = "variant task lifecycle";
            auto videoRecord = moved.front();
            videoRecord.kind = "video";
            auto mediaDirectory = library.MediaDirectory(videoRecord);
            require(library.RequestOptimization(videoRecord, "power-saver"),
                "video import did not create a durable optimization request");
            auto variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && !variantStatus.cancelled,
                "optimization request was not visible to the UI status model");
            library.PauseOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && variantStatus.paused,
                "pausing optimization lost its durable request");
            library.ResumeOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && !variantStatus.paused,
                "resuming optimization did not restore its queued state");
            fs::create_directories(mediaDirectory / L"Variants");
            auto partial = mediaDirectory / L"Variants" / L"power-saver-test.part.mp4";
            std::ofstream(partial, std::ios::binary) << "partial";
            library.CancelOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(!variantStatus.queued && !variantStatus.paused && variantStatus.cancelled &&
                !fs::exists(partial),
                "cancelling optimization did not clear its request and temporary output");
            require(library.RequestOptimization(videoRecord, "balanced"),
                "a cancelled optimization could not be requested again");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-test.mp4", std::ios::binary) << "derived-copy";
            std::ofstream(mediaDirectory / L"Variants" / L"power-saver-test.mp4", std::ios::binary) << "low-power";
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.files == 2 && variantStatus.bytes == 21,
                "generated optimization copies were not reported accurately");
            auto sourcePath = mediaDirectory / moved.front().fileName;
            library.SuppressOptimization(videoRecord, "balanced");
            library.DeleteVariantProfile(videoRecord, "balanced");
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.balancedSuppressed && variantStatus.files == 1 &&
                variantStatus.entries.front().mode == "power-saver" && fs::is_regular_file(sourcePath),
                "deleting one performance profile damaged the source or the other profile");
            require(library.RequestOptimization(videoRecord, "balanced") &&
                !library.VariantStatus(videoRecord).balancedSuppressed,
                "manual generation did not re-enable a deleted performance profile");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-test.mp4", std::ios::binary) << "derived-copy";
            library.SuppressOptimization(videoRecord, "balanced");
            library.SuppressOptimization(videoRecord, "power-saver");
            library.DeleteVariantProfile(videoRecord, "balanced");
            library.DeleteVariantProfile(videoRecord, "power-saver");
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.files == 0 && variantStatus.balancedSuppressed &&
                variantStatus.powerSaverSuppressed && fs::is_regular_file(sourcePath) &&
                !fs::exists(mediaDirectory / L"Variants"),
                "multi-profile deletion damaged the source or left selected copies behind");
            phase = "source-only delete";
            fs::create_directories(mediaDirectory / L"Variants");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-60-1280x720-v5.mp4",
                std::ios::binary) << "balanced-copy";
            std::ofstream(mediaDirectory / L"Variants" / L"power-saver-60-1280x720-v5.mp4",
                std::ios::binary) << "power-copy";
            auto metadata = motion::load_media(mediaDirectory / L"metadata.json");
            require(metadata.has_value(), "source-delete fixture lost its metadata");
            metadata->kind = "video";
            motion::save_media(mediaDirectory / L"metadata.json", *metadata);
            videoRecord = *metadata;
            library.DeleteSource(videoRecord);
            require(!library.SourceAvailable(videoRecord) &&
                fs::is_regular_file(mediaDirectory / L"metadata.json") &&
                fs::is_regular_file(mediaDirectory / L"poster.png") &&
                library.VariantStatus(videoRecord).entries.size() == 2,
                "deleting the source removed the wallpaper identity, poster, or retained copies");
            require(!library.RequestOptimization(videoRecord, "balanced"),
                "a source-less wallpaper accepted a new optimization request");
            library.DeleteVariantProfile(videoRecord, "balanced");
            bool lastCopyProtected{};
            try { library.DeleteVariantProfile(videoRecord, "power-saver"); }
            catch (...) { lastCopyProtected = true; }
            require(lastCopyProtected && library.VariantStatus(videoRecord).entries.size() == 1,
                "the final playable copy was not protected after source deletion");
            phase = "delete";
            library.Delete(moved.front());
            require(library.LoadMedia(second.id).empty(), "deleted media survived on disk");

            phase = "duplicate group";
            auto duplicate = first;
            duplicate.id = motion::new_id();
            auto duplicateRoot = library.WallpapersPath() / L"Groups" / motion::utf8_to_wide(duplicate.id);
            fs::create_directories(duplicateRoot / L"Videos");
            motion::save_group(duplicateRoot / L"group.json", duplicate);
            std::ofstream(duplicateRoot / L"keep.me") << "must survive";
            auto groups = library.LoadGroups().groups;
            auto sameNameCount = std::count_if(groups.begin(), groups.end(), [](auto const& group) { return _wcsicmp(group.name.c_str(), L"海景") == 0; });
            require(sameNameCount == 2, "duplicate group loading performed an implicit destructive merge");
            require(fs::is_regular_file(duplicateRoot / L"keep.me"), "duplicate group loading deleted an unrecognized file");
        } catch (std::exception const& error) {
            throw std::runtime_error(std::string(phase) + ": " + error.what());
        }
    }

    void media_catalog_and_optimization_storage_are_manageable(fs::path const& root)
    {
        auto libraryRoot = root / L"catalog-library";
        motion::app::MediaLibrary library(
            libraryRoot, motion::app::DeleteMode::Permanent);
        library.EnsureDirectories();
        auto beachGroup = library.CreateGroup(L"海滩", {});
        auto nightGroup = library.CreateGroup(L"夜间", { beachGroup });

        auto createMedia = [&](motion::GroupMetadata const& group,
            std::wstring name, std::wstring hash, std::string kind = "video") {
            motion::MediaMetadata media;
            media.id = motion::new_id();
            media.groupId = group.id;
            media.name = std::move(name);
            media.originalName = media.name + (kind == "video" ? L".mp4" : L".png");
            media.fileName = kind == "video" ? L"source.mp4" : L"source.png";
            media.kind = std::move(kind);
            media.sha256 = std::move(hash);
            media.sizeBytes = 6;
            media.revision = 1;
            media.importedAt = media.updatedAt = motion::timestamp_utc();
            auto directory = library.MediaDirectory(media);
            fs::create_directories(directory);
            std::ofstream(directory / media.fileName, std::ios::binary) << "source";
            motion::save_media(directory / L"metadata.json", media);
            return media;
        };

        constexpr wchar_t sourceSha256[] =
            L"41cf6794ba4200b839c53531555f0f3998df4cbb01a4d5cb0b94e3ca5e23947d";
        auto beach = createMedia(beachGroup, L"夏日海滩", sourceSha256);
        auto duplicate = createMedia(nightGroup, L"海边副本", sourceSha256);
        auto city = createMedia(nightGroup, L"城市灯光", L"bbbbbbbb");
        library.SetFavorite(std::vector<motion::MediaMetadata>{ beach, city }, true);
        library.AddTags({ beach, duplicate }, { L" 海滩 ", L"夜间", L"海滩" });
        library.SetTags(city, { L"城市", L"工作" });

        motion::app::MediaQuery query;
        query.text = L"夏日";
        query.favoritesOnly = true;
        auto favoriteNight = library.QueryMedia(query);
        require(favoriteNight.size() == 1 && favoriteNight.front().media.id == beach.id &&
            favoriteNight.front().media.tags.size() == 2,
            "catalog text/favorite search or normalized tag persistence failed");
        query = {};
        query.groupId = nightGroup.id;
        query.tags = { L"海滩" };
        auto taggedInGroup = library.QueryMedia(query);
        require(taggedInGroup.size() == 1 && taggedInGroup.front().media.id == duplicate.id,
            "cross-field catalog filtering ignored its group or tag constraint");

        library.SetFavorite(duplicate, true);
        library.AddTags({ duplicate }, { L"收藏" });
        auto duplicates = library.FindDuplicateMedia();
        require(duplicates.size() == 1 && duplicates.front().items.size() == 2,
            "identical content across groups was not reported as a repairable duplicate");
        auto merged = library.MergeDuplicateMedia(beach, duplicate);
        require(merged.favorite && merged.tags.size() == 3 &&
            library.QueryMedia().size() == 2 && library.FindDuplicateMedia().empty(),
            "duplicate repair did not merge user metadata or remove only the duplicate");

        auto refreshed = library.QueryMedia();
        auto refreshedBeach = std::find_if(refreshed.begin(), refreshed.end(),
            [&](auto const& item) { return item.media.id == beach.id; });
        auto refreshedCity = std::find_if(refreshed.begin(), refreshed.end(),
            [&](auto const& item) { return item.media.id == city.id; });
        require(refreshedBeach != refreshed.end() && refreshedCity != refreshed.end(),
            "catalog refresh lost a non-duplicate item");
        beach = refreshedBeach->media;
        city = refreshedCity->media;

        auto addVariant = [&](motion::MediaMetadata const& media,
            std::wstring const& name, size_t bytes) {
            auto variants = library.MediaDirectory(media) / L"Variants";
            fs::create_directories(variants);
            std::ofstream output(variants / name, std::ios::binary);
            output << std::string(bytes, 'v');
        };
        addVariant(beach, L"balanced-test.mp4", 11);
        addVariant(city, L"power-saver-test.mp4", 9);
        auto sourceLess = createMedia(nightGroup, L"仅优化版本", L"cccccccc");
        addVariant(sourceLess, L"balanced-test.mp4", 7);
        fs::remove(library.MediaDirectory(sourceLess) / sourceLess.fileName);

        auto storage = library.InspectOptimizationStorage();
        require(storage.bytes == 27 && storage.files == 3 &&
            storage.reclaimableBytes == 20,
            "optimization storage summary did not separate safe reclaimable bytes");
        auto cleanup = library.ReleaseOptimizationStorage({ beach.id });
        require(cleanup.cleanedMedia == 1 && cleanup.freedBytes == 9 &&
            cleanup.skippedProtected == 1 && cleanup.skippedSourceLess == 1 &&
            cleanup.after.bytes == 18 &&
            fs::is_regular_file(library.MediaDirectory(beach) / L"Variants" /
                L"balanced-test.mp4") &&
            fs::is_regular_file(library.MediaDirectory(sourceLess) / L"Variants" /
                L"balanced-test.mp4"),
            "one-click optimization cleanup removed protected or source-less playback data");

        auto legacyDirectory = libraryRoot / L"Groups" /
            motion::utf8_to_wide(nightGroup.id) / L"Videos" /
            motion::utf8_to_wide(motion::new_id());
        fs::create_directories(legacyDirectory);
        auto legacyId = motion::wide_to_utf8(legacyDirectory.filename().wstring());
        std::ofstream(legacyDirectory / L"source.png", std::ios::binary) << "legacy";
        std::ofstream(legacyDirectory / L"metadata.json", std::ios::binary)
            << "{\"version\":2,\"id\":\"" << legacyId
            << "\",\"groupId\":\"" << nightGroup.id
            << "\",\"name\":\"legacy\",\"originalName\":\"legacy.png\","
               "\"fileName\":\"source.png\",\"kind\":\"image\","
               "\"coverFileName\":\"\",\"sha256\":\"dddddddd\","
               "\"sizeBytes\":6,\"revision\":1,\"importedAt\":\"\","
               "\"updatedAt\":\"\"}";
        auto legacy = motion::load_media(legacyDirectory / L"metadata.json");
        require(legacy && legacy->version == motion::media_schema_version &&
            !legacy->favorite && legacy->tags.empty(),
            "v2 media metadata did not migrate safely to empty catalog fields");
    }

    void optimization_eta_round_trips(fs::path const& root)
    {
        auto mediaDirectory = root / L"optimization-eta";
        fs::create_directories(mediaDirectory);
        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "ETA fixture request could not be persisted");
        auto request = motion::read_variant_generation_request(mediaDirectory);
        require(motion::write_variant_progress_if_current(mediaDirectory, request,
                motion::VariantProgressState::generating, 40, true, 125, true),
            "optimization ETA could not be published for the current request");
        auto status = motion::inspect_variant_cache(mediaDirectory);
        require(status.generating && status.progressKnown && status.progressPercent == 40 &&
            status.estimatedRemainingKnown && status.estimatedRemainingSeconds == 125,
            "optimization ETA was not exposed through the status model");
        require(motion::pause_variant_generation(mediaDirectory),
            "ETA fixture could not be paused");
        status = motion::inspect_variant_cache(mediaDirectory);
        require(status.paused && !status.estimatedRemainingKnown,
            "a paused/restartable optimization retained a stale ETA");
        auto legacy = motion::parse_variant_progress("v2|balanced|" + request.requestId +
            "|generating|22");
        require(legacy && legacy->percent == 22 && !legacy->estimatedRemainingKnown,
            "the ETA progress extension rejected a valid v2 progress record");
    }

    void xaml_events_are_bound_to_handlers()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream xamlFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml", std::ios::binary);
        std::ifstream cppFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp", std::ios::binary);
        std::ifstream headerFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.h", std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        require(static_cast<bool>(xamlFile) && static_cast<bool>(cppFile) && static_cast<bool>(headerFile) && static_cast<bool>(agentFile),
            "UI or Agent source files were not found");
        std::string xaml((std::istreambuf_iterator<char>(xamlFile)), {});
        std::string cpp((std::istreambuf_iterator<char>(cppFile)), {});
        std::string header((std::istreambuf_iterator<char>(headerFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        std::regex event(R"event((?:Click|SelectionChanged|TextChanged|RightTapped|DragItemsStarting|DragItemsCompleted)="([A-Za-z0-9_]+)")event");
        for (std::sregex_iterator found(xaml.begin(), xaml.end(), event), end; found != end; ++found) {
            auto handler = (*found)[1].str();
            require(cpp.find("MainWindow::" + handler + "(") != std::string::npos, "XAML event references a missing handler");
        }
        std::regex declaredEvent(R"event(void ([A-Za-z0-9_]+)\(Windows::Foundation::IInspectable const&)event");
        for (std::sregex_iterator found(header.begin(), header.end(), declaredEvent), end; found != end; ++found) {
            auto handler = (*found)[1].str();
            require(xaml.find("=\"" + handler + "\"") != std::string::npos, "declared UI event handler is no longer bound in XAML");
        }
        require(cpp.find("SelectWallpaperForTarget(media.groupId, media.id, selectedDisplayId)") != std::string::npos,
            "wallpaper card selection no longer updates the persisted selection");
        require(cpp.find("profileContext + L\"，保留选择\"") != std::string::npos &&
            cpp.find("profileContext + L\"，\" + actionLabel") != std::string::npos &&
            cpp.find("item.media.name + L\"，源文件，保留选择\"") != std::string::npos,
            "performance-copy controls no longer expose per-media accessible names");
        require(cpp.find("motion::try_load_runtime(path, runtime)") != std::string::npos,
            "wallpaper UI no longer consumes the Agent's acknowledged runtime selection");
        require(xaml.find("x:Name=\"VariantsPage\"") != std::string::npos &&
            xaml.find("Click=\"Variants_Click\"") != std::string::npos,
            "the global performance-copy page is no longer reachable");
        require(xaml.find("GenerateVariantButton") == std::string::npos &&
            xaml.find("DeleteVariantsButton") == std::string::npos,
            "performance-copy actions leaked back into a wallpaper group page");
        require(cpp.find("enum class AppPage") == std::string::npos,
            "page navigation state was duplicated in the implementation file");
        require(agent.find("save_settings(") == std::string::npos,
            "Agent became a second writer of the user settings file");
        require(agent.find("runtime_selection_can_publish(") != std::string::npos &&
            agent.find("publishRuntime(groupId, mediaId, decode.path, decode.reason)") != std::string::npos,
            "wallpaper runtime state is published before the Renderer ACK");
        require(agent.find("RendererPool") != std::string::npos && agent.find("display_media_targets") != std::string::npos,
            "per-display renderer routing is no longer active");
        require(agent.find("EnumWindows(") != std::string::npos && agent.find("desktop_covered()") != std::string::npos,
            "fullscreen coverage regressed to foreground-window-only detection");
    }

    void destructive_ui_waits_for_agent_quiescence()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream windowFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp",
            std::ios::binary);
        require(static_cast<bool>(windowFile), "MainWindow source file was not found");
        std::string window((std::istreambuf_iterator<char>(windowFile)), {});
        auto slice = [&](char const* begin, char const* end) {
            auto first = window.find(begin);
            auto last = window.find(end, first);
            require(first != std::string::npos && last != std::string::npos && first < last,
                "destructive coroutine source boundaries were not found");
            return window.substr(first, last - first);
        };
        auto group = slice("MainWindow::DeleteGroup(", "MainWindow::ImportVideo_Click(");
        auto variants = slice("MainWindow::DeleteVariantProfiles(", "MainWindow::DeleteSource(");
        auto source = slice("MainWindow::DeleteSource(", "MainWindow::DeleteMedia_Click(");
        auto media = slice("MainWindow::DeleteMedia(", "MainWindow::OpenLibrary_Click(");
        auto move = slice("MainWindow::MoveMedia(", "MainWindow::RandomInterval_Changed(");
        auto coordinated = [](std::string const& body, char const* destructiveCall) {
            auto request = body.find("RequestAndWait(std::chrono::seconds(20))");
            auto destructive = body.find(destructiveCall);
            auto resume = body.find("ResumeAndWait(std::chrono::seconds(20))");
            return request != std::string::npos && destructive != std::string::npos &&
                resume != std::string::npos && request < destructive && destructive < resume;
        };
        require(coordinated(group, "library->DeleteGroup(group)") &&
            coordinated(variants, "library->DeleteVariantProfiles(media, modes)") &&
            coordinated(source, "library->DeleteSource(media)") &&
            coordinated(media, "library->Delete(media)") &&
            coordinated(move, "library->Move(media, targetId)"),
            "a destructive or path-changing UI flow can mutate before the Agent acknowledges quiescence");
        require(group.find("activeLibraryMigrationPause = agentPause") != std::string::npos &&
            group.find("SetLibraryMigrationUi(true)") != std::string::npos &&
            group.find("if (!deleted)") != std::string::npos &&
            group.find("settings = std::move(previousSettings)") != std::string::npos,
            "group deletion does not gate concurrent UI work or roll settings back after failure");
        require(variants.find("SuppressOptimization(media") == std::string::npos &&
            window.find("resume_after(std::chrono::milliseconds(1200))") == std::string::npos,
            "variant deletion still pre-suppresses files or relies on a fixed sleep");
        auto migrationUi = slice("MainWindow::SetLibraryMigrationUi(", "MainWindow::LoadGroups(");
        require(migrationUi.find("Content().as<FrameworkElement>().IsHitTestVisible(!migrating)") != std::string::npos &&
            migrationUi.find("SettingsPage().IsEnabled(!migrating)") != std::string::npos &&
            migrationUi.find("VariantsPage().IsEnabled(!migrating)") != std::string::npos &&
            migrationUi.find("WallpaperPage().IsEnabled(!migrating)") != std::string::npos &&
            migrationUi.find("SettingsNavButton().IsEnabled(!migrating)") != std::string::npos &&
            migrationUi.find("VariantsNavButton().IsEnabled(!migrating)") != std::string::npos,
            "an Agent-quiesced operation leaves a settings page or navigation path interactive");
    }

    void windows_app_sdk_dependencies_are_release_safe()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream projectFile(
            sourceRoot / L"MotionWallpaper.App" / L"MotionWallpaper.App.vcxproj", std::ios::binary);
        std::string project((std::istreambuf_iterator<char>(projectFile)), {});
        require(project.find("Microsoft.WindowsAppSDK.Foundation\" Version=\"1.8.260803002") != std::string::npos &&
            project.find("Microsoft.WindowsAppSDK.InteractiveExperiences\" Version=\"1.8.260708001") != std::string::npos &&
            project.find("Microsoft.WindowsAppSDK.WinUI\" Version=\"1.8.260803003") != std::string::npos,
            "release build no longer pins the audited stable Windows App SDK 1.8 components");
        require(project.find("Include=\"Microsoft.WindowsAppSDK\"") == std::string::npos &&
            project.find("Version=\"2.") == std::string::npos,
            "an aggregate or engineering-preview Windows App SDK dependency entered the release project");
    }

    void display_topology_uses_physical_pixels()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream rendererFile(sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp", std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        std::ifstream awarenessFile(sourceRoot / L"MotionWallpaper.Common" / L"DisplayAwareness.h", std::ios::binary);
        require(rendererFile && agentFile && awarenessFile, "display topology source files were not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        std::string awareness((std::istreambuf_iterator<char>(awarenessFile)), {});
        require(awareness.find("DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2") != std::string::npos,
            "display coordinates regressed to DPI-virtualized logical pixels");
        require(renderer.find("motion::enable_per_monitor_dpi_awareness();") != std::string::npos &&
            agent.find("motion::enable_per_monitor_dpi_awareness();") != std::string::npos,
            "Renderer and Agent no longer share the same physical-pixel coordinate space");
        require(renderer.find("case WM_DISPLAYCHANGE:") != std::string::npos &&
            renderer.find("case WM_DPICHANGED:") != std::string::npos,
            "Renderer no longer rebuilds after monitor resolution or DPI changes");
    }

    void mixed_resolution_displays_keep_independent_physical_bounds()
    {
        RECT laptop{ 0, 0, 2560, 1600 };
        RECT external{ 2560, 80, 5120, 1520 };
        require(laptop.right - laptop.left == 2560 && laptop.bottom - laptop.top == 1600,
            "laptop output no longer retains its own physical size");
        require(external.right - external.left == 2560 && external.bottom - external.top == 1440,
            "external output was incorrectly stretched to the virtual-desktop height");
        require(external.left == 2560 && external.top == 80,
            "secondary output lost its independent physical origin");
    }

    void active_displays_have_stable_physical_targets()
    {
        auto displays = motion::enumerate_displays();
        require(!displays.empty(), "Windows did not expose an active display target");
        size_t primaryCount{};
        std::vector<std::string> ids;
        for (auto const& display : displays) {
            require(!display.id.empty() && !display.deviceName.empty(), "active display has no stable identity");
            require(display.bounds.right > display.bounds.left && display.bounds.bottom > display.bounds.top,
                "active display has invalid physical bounds");
            require(std::find(ids.begin(), ids.end(), display.id) == ids.end(), "active display identities are not unique");
            ids.push_back(display.id);
            if (display.primary) ++primaryCount;
            auto resolved = motion::find_display_bounds(display.deviceName);
            require(resolved.has_value() && EqualRect(&*resolved, &display.bounds),
                "display device did not resolve back to its physical bounds");
        }
        require(primaryCount == 1, "display topology does not contain exactly one primary monitor");
    }
}

int wmain(int argc, wchar_t** argv)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (argc == 5 && std::wstring_view(argv[1]) == L"--leave-library-migration") {
        auto identity = motion::capture_media_library_trust(argv[2]);
        if (!identity) return 70;
        auto transaction = motion::app::LibraryMigrationTransaction::Begin(
            argv[2], argv[3], *identity);
        auto phase = std::wstring_view(argv[4]);
        if (phase == L"copying") {
            transaction->CopyAndVerify([](uint64_t copied, uint64_t) {
                if (copied) ExitProcess(71);
            });
            return 72;
        }
        transaction->CopyAndVerify();
        if (phase == L"committed") transaction->CommitPreparedTarget();
        ExitProcess(71);
    }
    if (argc == 8 && (std::wstring_view(argv[1]) == L"--transcode-video" ||
        std::wstring_view(argv[1]) == L"--transcode-main10-video" ||
        std::wstring_view(argv[1]) == L"--transcode-cpu-video")) {
        bool cpuPlayback = std::wstring_view(argv[1]) == L"--transcode-cpu-video";
        bool main10 = std::wstring_view(argv[1]) == L"--transcode-main10-video";
        std::wstring error;
        auto result = motion::agent::transcode_video(
            argv[2], argv[3], argv[4], static_cast<uint32_t>(_wtoi(argv[5])),
            static_cast<uint32_t>(_wtoi(argv[6])), static_cast<uint32_t>(_wtoi(argv[7])),
            [] { return motion::agent::VideoTranscodeControl::running; }, error,
            nullptr, !main10, cpuPlayback, 0,
            [](motion::agent::VideoTranscodeProgress const& value) {
                std::wcout << L"progress attempt=" << value.attempt << L" backend=" <<
                    static_cast<uint32_t>(value.backend) << L" percent=" << value.percent <<
                    L" processed_us=" << value.processedMicroseconds << L" duration_us=" <<
                    value.durationMicroseconds << L" started=" << value.attemptStarted << L'\n';
            });
        std::wcout << static_cast<int>(result) << L" " << error << L'\n';
        return result == motion::agent::VideoTranscodeResult::succeeded ? 0 : 3;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--write-settings") {
        motion::Settings settings;
        settings.idleTimeoutSeconds = 777;
        settings.selectedGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        settings.selectedMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        for (int index = 0; index < 100; ++index) motion::save_settings(argv[2], settings);
        return 0;
    }
    if (argc == 3 && (std::wstring_view(argv[1]) == L"--write-settings-a" ||
        std::wstring_view(argv[1]) == L"--write-settings-b")) {
        bool first = std::wstring_view(argv[1]) == L"--write-settings-a";
        motion::Settings settings;
        settings.idleTimeoutSeconds = first ? 701 : 702;
        settings.selectedGroupId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        settings.selectedMediaId = first ? "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa" :
            "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        for (int index = 0; index < 200; ++index) motion::save_settings(argv[2], settings);
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--write-runtime") {
        motion::RuntimeState runtime;
        runtime.activeGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        runtime.activeMediaId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        runtime.decodePath = "software-fallback";
        runtime.decodeReason = "fallback-no-hardware-decoder";
        for (int index = 0; index < 100; ++index) motion::save_runtime(argv[2], runtime);
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--run-variant-worker") {
        fs::path mediaDirectory = argv[2];
        auto request = motion::read_variant_generation_request(mediaDirectory);
        if (!request || !motion::write_variant_progress_if_current(mediaDirectory,
            request, motion::VariantProgressState::generating, 35, true, 4, true)) return 80;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::error_code markerError;
        while (!fs::is_regular_file(mediaDirectory / L".test-worker-continue", markerError)) {
            markerError.clear();
            if (std::chrono::steady_clock::now() >= deadline) return 81;
            Sleep(10);
        }
        if (!motion::write_variant_progress_if_current(mediaDirectory,
            request, motion::VariantProgressState::generating, 100, true, 0, true)) return 82;
        return motion::complete_variant_generation(mediaDirectory, request) ? 0 : 83;
    }
    auto root = fs::temp_directory_path() / (L"MotionWallpaper.Tests." + motion::utf8_to_wide(motion::new_id()));
    char const* currentTest = "startup";
#define RUN_TEST(expression) do { \
        currentTest = #expression; \
        std::cerr << "[ RUN      ] " << currentTest << std::endl; \
        expression; \
        std::cerr << "[       OK ] " << currentTest << std::endl; \
    } while (false)
    try {
        fs::create_directories(root);
        RUN_TEST(application_data_location_preserves_portable_and_legacy_libraries(root));
        RUN_TEST(settings_round_trip_clears_empty_values(root));
        RUN_TEST(scene_profiles_and_layout_round_trip(root));
        RUN_TEST(legacy_settings_are_migrated(root));
        RUN_TEST(coupled_lock_and_display_off_settings_are_migrated(root));
        RUN_TEST(unsafe_media_paths_are_rejected(root));
        RUN_TEST(desktop_state_is_deterministic());
        RUN_TEST(presentation_state_has_explicit_priorities());
        RUN_TEST(tray_controls_preview_and_cycle_without_polling());
        RUN_TEST(identifiers_are_path_safe());
        RUN_TEST(future_settings_are_rejected(root));
        RUN_TEST(custom_library_settings_require_owned_safe_roots(root));
        RUN_TEST(persisted_library_identity_rejects_same_path_replacement(root));
        RUN_TEST(media_library_trust_detects_runtime_replacement(root));
        RUN_TEST(agent_settings_fail_closed_until_recovery(root));
        RUN_TEST(migration_owner_channel_recovers_only_orphaned_requests());
        RUN_TEST(media_activity_suspends_idle_time());
        RUN_TEST(audio_allows_screensaver_but_defers_automatic_lock());
        RUN_TEST(external_media_remains_authoritative_during_own_screensaver());
        RUN_TEST(display_off_waits_for_the_post_lock_delay());
        RUN_TEST(fullscreen_coverage_is_not_limited_to_foreground());
        RUN_TEST(normal_pause_keeps_decoder_hot());
        RUN_TEST(stable_agent_states_do_not_poll_at_twenty_hertz());
        RUN_TEST(battery_power_pauses_optional_variant_generation());
        RUN_TEST(active_playback_waits_for_selected_performance_copy());
        RUN_TEST(pending_performance_copy_preserves_the_presented_frame());
        RUN_TEST(variant_progress_round_trips_and_resets(root));
        RUN_TEST(variant_retention_honors_runtime_leases(root));
        RUN_TEST(motion::tests::library_backup_restores_verified_snapshot(root, require));
        RUN_TEST(motion::tests::library_restore_recovers_every_durable_crash_boundary(root, require));
        RUN_TEST(motion::tests::library_restore_recovers_offline_library_and_corrupt_settings(root, require));
        RUN_TEST(runtime_variant_cleanup_is_lease_safe_and_atomic());
        RUN_TEST(first_freeze_keeps_its_compaction_surface());
        RUN_TEST(playback_capability_only_degrades_software_devices());
        RUN_TEST(software_presentation_governor_recovers_without_catchup_bursts());
        RUN_TEST(screensaver_pause_returns_window_to_desktop());
        RUN_TEST(desktop_host_must_cover_the_virtual_screen());
        RUN_TEST(manual_selection_wins_over_group_randomization());
        RUN_TEST(identical_media_share_one_renderer());
        RUN_TEST(video_variant_policy_preserves_quality_priority());
        RUN_TEST(variant_requests_use_last_writer_wins(root));
        RUN_TEST(same_mode_variant_retry_rejects_stale_worker(root));
        RUN_TEST(video_transcoder_fails_closed_without_backend(root));
        RUN_TEST(media_foundation_candidate_probe_decodes_a_real_first_frame(root));
        RUN_TEST(video_transcoder_orders_vendor_backends_and_bounds_software_fallback());
        RUN_TEST(frame_scheduler_uses_real_interval());
        RUN_TEST(adapter_policy_preserves_heavy_video_throughput());
        RUN_TEST(renderer_ack_channels_are_isolated());
        RUN_TEST(decode_modes_have_distinct_fallback_contracts());
        RUN_TEST(selected_media_reaches_real_renderer_first_frame(root));
        RUN_TEST(renderer_crash_recovery_reaches_first_frame_again(root));
        RUN_TEST(renderer_display_change_exit_and_relaunch_is_cross_process(root));
        RUN_TEST(real_renderer_enters_screensaver_and_returns_on_wake(root));
        RUN_TEST(safe_agent_system_event_messages_trigger_recovery());
        RUN_TEST(screensaver_input_wake_is_immediate_in_runtime_state_machine());
        RUN_TEST(renderer_process_uses_typed_acks(root));
        RUN_TEST(renderer_exits_when_agent_pipe_closes(root));
        RUN_TEST(performance_copy_queue_is_observable_across_processes(root));
        RUN_TEST(settings_and_runtime_have_single_writers(root));
        RUN_TEST(automatic_decode_runtime_round_trips(root));
        RUN_TEST(display_runtime_status_and_control_round_trip(root));
        RUN_TEST(display_runtime_status_has_trustworthy_precedence());
        RUN_TEST(agent_consumes_renderer_recovery_commands());
        RUN_TEST(concurrent_settings_writers_never_publish_torn_json(root));
        RUN_TEST(corrupt_files_preserve_last_known_good(root));
        RUN_TEST(media_library_mutations_revalidate_persistent_identity(root));
        RUN_TEST(interrupted_variant_deletion_is_recovered(root));
        RUN_TEST(interrupted_library_transactions_recover_without_overwrite(root));
        RUN_TEST(media_library_operations_are_safe(root));
        RUN_TEST(media_catalog_and_optimization_storage_are_manageable(root));
        RUN_TEST(optimization_eta_round_trips(root));
        RUN_TEST(library_migration_is_verified_and_ownership_scoped(root));
        RUN_TEST(xaml_events_are_bound_to_handlers());
        RUN_TEST(destructive_ui_waits_for_agent_quiescence());
        RUN_TEST(windows_app_sdk_dependencies_are_release_safe());
        RUN_TEST(display_topology_uses_physical_pixels());
        RUN_TEST(mixed_resolution_displays_keep_independent_physical_bounds());
        RUN_TEST(active_displays_have_stable_physical_targets());
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::wcout << L"MotionWallpaper native tests passed\n";
        return 0;
    } catch (std::exception const& error) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cerr << "MotionWallpaper native tests failed in " << currentTest << ": " << error.what() << '\n';
        return 1;
    }
#undef RUN_TEST
}
