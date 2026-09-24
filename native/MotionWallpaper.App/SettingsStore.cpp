#include "pch.h"
#include "SettingsStore.h"

namespace motion::app
{
    bool SettingsStore::ApplyStartupPreference(bool enabled) const noexcept
    {
        constexpr wchar_t key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (!enabled) {
            auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"MotionWallpaper");
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }
        std::wstring command = L"\"" + agentPath_.wstring() + L"\"";
        auto result = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"MotionWallpaper", REG_SZ,
            command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        return result == ERROR_SUCCESS;
    }

    motion::Settings SettingsStore::Load(bool* mediaLibraryAvailable) const
    {
        if (mediaLibraryAvailable) *mediaLibraryAvailable = true;
        std::error_code fileError;
        bool exists = std::filesystem::exists(path_, fileError);
        if (fileError) throw std::system_error(fileError);

        motion::Settings settings;
        if (exists) {
            auto status = motion::load_settings_file(path_, settings);
            // Corrupt files and future schemas are intentionally preserved.
            // An older binary must never replace settings it cannot understand
            // with a freshly serialized default document.
            if (status == motion::SettingsFileStatus::invalid ||
                status == motion::SettingsFileStatus::missing) {
                throw std::runtime_error("settings schema is unsupported or corrupt");
            }
            if (status == motion::SettingsFileStatus::libraryUnavailable) {
                if (mediaLibraryAvailable) *mediaLibraryAvailable = false;
                ApplyStartupPreference(settings.startWithWindows);
                return settings;
            }
        }
        // Create the first-run document or canonicalize a schema version that
        // this binary successfully parsed and migrated in memory.
        motion::save_settings_with_playback_merge(path_, settings);
        ApplyStartupPreference(settings.startWithWindows);
        return settings;
    }

    bool SettingsStore::Save(motion::Settings& settings, bool activePlaybackExplicit) const
    {
        motion::save_settings_with_playback_merge(path_, settings, activePlaybackExplicit);
        ApplyStartupPreference(settings.startWithWindows);
        return motion::notify_settings_changed();
    }
}
