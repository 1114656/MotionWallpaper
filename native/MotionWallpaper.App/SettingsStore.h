#pragma once

#include "../MotionWallpaper.Common/Common.h"

namespace motion::app
{
    class SettingsStore
    {
    public:
        SettingsStore(std::filesystem::path dataRoot, std::filesystem::path applicationRoot)
            : path_(std::move(dataRoot) / L"Config" / L"settings.json"),
              agentPath_(std::move(applicationRoot) / L"motionwallpaper-agent.exe") {}
        motion::Settings Load(bool* mediaLibraryAvailable = nullptr) const;
        bool Save(motion::Settings& settings, bool activePlaybackExplicit = false) const;
        bool ApplyStartupPreference(bool enabled) const noexcept;
    private:
        std::filesystem::path path_;
        std::filesystem::path agentPath_;
    };
}
